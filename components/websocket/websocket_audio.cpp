#include "websocket_audio.h"
#include "websocket.h"
#include "websocket_event.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";

// AudioEngine conversation contract: 20 ms = 320 samples @ 16 kHz PCM16 mono.
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAMES_PER_MESSAGE = 5;
static constexpr size_t MESSAGE_SAMPLES = FRAME_SAMPLES * FRAMES_PER_MESSAGE;
static constexpr size_t MESSAGE_BYTES = MESSAGE_SAMPLES * sizeof(int16_t);
static constexpr uint32_t TASK_STACK = 6144;
static constexpr UBaseType_t TASK_PRIORITY = 5;
static constexpr int64_t SEND_WARN_US = 80000;

// Repo3 pattern: MIC capture never performs the potentially blocking
// WebSocket write directly. Capture enqueues PCM and a dedicated TX worker
// owns the socket so MIC capture remains alive across model turns.
static constexpr size_t TX_QUEUE_LENGTH = 16;
static constexpr uint32_t TX_TASK_STACK = 8192;
static constexpr UBaseType_t TX_TASK_PRIORITY = 4;

typedef struct {
    uint8_t *data;
    size_t len;
} tx_audio_item_t;

static QueueHandle_t s_tx_queue = nullptr;
static TaskHandle_t s_tx_task = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;

static void websocket_audio_tx_task(void *)
{
    tx_audio_item_t item{};
    ESP_LOGI(TAG, "Audio TX worker START: queue=%u", (unsigned)TX_QUEUE_LENGTH);

    for (;;) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!item.data || item.len == 0) {
            free(item.data);
            item = {};
            continue;
        }

        if (!websocket_is_connected() || !websocket_event_gemini_ready()) {
            free(item.data);
            item = {};
            continue;
        }

        const int64_t send_start_us = esp_timer_get_time();
        const esp_err_t err = websocket_send_text(
            reinterpret_cast<const char *>(item.data), item.len);
        const int64_t send_elapsed_us = esp_timer_get_time() - send_start_us;

        if (send_elapsed_us >= SEND_WARN_US) {
            ESP_LOGW(TAG, "Audio TX socket write lambat: %lld ms, json=%uB",
                     (long long)(send_elapsed_us / 1000),
                     (unsigned)item.len);
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Audio TX gagal: %s", esp_err_to_name(err));
        }

        free(item.data);
        item = {};
    }
}

static bool ensure_tx_worker(void)
{
    if (s_tx_task) return true;

    if (!s_tx_queue) {
        s_tx_queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(tx_audio_item_t));
        if (!s_tx_queue) {
            ESP_LOGE(TAG, "Gagal membuat audio TX queue");
            return false;
        }
    }

    if (xTaskCreate(websocket_audio_tx_task,
                    "ws_audio_tx",
                    TX_TASK_STACK,
                    nullptr,
                    TX_TASK_PRIORITY,
                    &s_tx_task) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat audio TX worker");
        return false;
    }

    return true;
}

static bool enqueue_audio_json(char *json, size_t json_len)
{
    if (!json || json_len == 0 || !s_tx_queue) {
        free(json);
        return false;
    }

    tx_audio_item_t item{};
    item.data = reinterpret_cast<uint8_t *>(json);
    item.len = json_len;

    if (xQueueSend(s_tx_queue, &item, 0) == pdTRUE) {
        return true;
    }

    // Keep the newest microphone data. If the socket is temporarily slow,
    // discard one oldest queued frame rather than blocking the MIC task.
    tx_audio_item_t stale{};
    if (xQueueReceive(s_tx_queue, &stale, 0) == pdTRUE) {
        free(stale.data);
        if (xQueueSend(s_tx_queue, &item, 0) == pdTRUE) {
            ESP_LOGW(TAG, "Audio TX queue penuh: 1 frame lama dibuang");
            return true;
        }
    }

    free(item.data);
    ESP_LOGW(TAG, "Audio TX queue penuh: frame terbaru dibuang");
    return false;
}

static void websocket_audio_task(void *)
{
    static int16_t message_pcm[MESSAGE_SAMPLES];
    static int16_t frame_pcm[FRAME_SAMPLES];
    size_t frames_collected = 0;

    ESP_LOGI(TAG,
             "Audio uplink worker START: frame=%u samples/%uB, message=%u samples/%uB",
             (unsigned)FRAME_SAMPLES,
             (unsigned)(FRAME_SAMPLES * sizeof(int16_t)),
             (unsigned)MESSAGE_SAMPLES,
             (unsigned)MESSAGE_BYTES);

    while (s_running) {
        if (!websocket_is_connected() || !websocket_event_gemini_ready()) {
            frames_collected = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!audio_engine_read_mic_frame(frame_pcm, FRAME_SAMPLES, 100)) {
            continue;
        }

        memcpy(&message_pcm[frames_collected * FRAME_SAMPLES],
               frame_pcm, FRAME_SAMPLES * sizeof(int16_t));
        frames_collected++;

        if (frames_collected < FRAMES_PER_MESSAGE) {
            continue;
        }

        char *json = nullptr;
        size_t json_len = 0;
        if (!gemini_protocol_build_realtime_audio(
                message_pcm, MESSAGE_SAMPLES, &json, &json_len)) {
            ESP_LOGE(TAG, "Gagal membuat realtime audio message");
            frames_collected = 0;
            continue;
        }

        frames_collected = 0;
        (void)enqueue_audio_json(json, json_len);
    }

    s_task = nullptr;
    ESP_LOGI(TAG, "Audio uplink worker STOP");
    vTaskDelete(nullptr);
}

bool websocket_audio_start(void)
{
    if (s_running) return true;
    if (!audio_engine_conversation_active()) {
        ESP_LOGW(TAG, "Conversation AudioEngine belum aktif");
        return false;
    }
    if (!ensure_tx_worker()) return false;

    s_running = true;
    BaseType_t result = xTaskCreate(
        websocket_audio_task,
        "ws_audio",
        TASK_STACK,
        nullptr,
        TASK_PRIORITY,
        &s_task);

    if (result != pdPASS) {
        s_running = false;
        s_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat task ws_audio");
        return false;
    }

    return true;
}

void websocket_audio_stop(void)
{
    s_running = false;
}

bool websocket_audio_running(void)
{
    return s_running;
}
