#include "websocket_audio.h"
#include "websocket.h"
#include "websocket_event.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";

// AudioEngine owns MIC and prepares 20 ms PCM16 frames (320 samples @ 16 kHz).
// This file only bridges prepared AudioEngine frames to Gemini transport.
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAMES_PER_MESSAGE = 5;
static constexpr size_t MESSAGE_SAMPLES = FRAME_SAMPLES * FRAMES_PER_MESSAGE;
static constexpr size_t TX_QUEUE_DEPTH = 4;
static constexpr uint32_t BRIDGE_TASK_STACK = 4096;
static constexpr uint32_t TX_TASK_STACK = 4096;
static constexpr UBaseType_t TASK_PRIORITY = 5;
static constexpr int64_t SEND_WARN_US = 80000;
static constexpr uint32_t STOP_WAIT_MS = 300;

struct TxMessage {
    char *json;
    size_t len;
    int64_t queued_at_us;
};

static StaticQueue_t s_tx_queue_storage;
static TxMessage s_tx_queue_buffer[TX_QUEUE_DEPTH];
static QueueHandle_t s_tx_queue = nullptr;
static TaskHandle_t s_bridge_task = nullptr;
static TaskHandle_t s_tx_task = nullptr;
static volatile bool s_running = false;

static void tx_queue_flush(void)
{
    if (!s_tx_queue) return;

    TxMessage message{};
    while (xQueueReceive(s_tx_queue, &message, 0) == pdTRUE) {
        free(message.json);
    }
}

static bool tx_queue_push(char *json, size_t json_len)
{
    if (!json || json_len == 0 || !s_tx_queue) {
        free(json);
        return false;
    }

    TxMessage message{json, json_len, esp_timer_get_time()};
    if (xQueueSend(s_tx_queue, &message, 0) == pdTRUE) {
        return true;
    }

    // Audio is realtime: never build unlimited backlog. Drop the oldest
    // pending message and keep the newest captured audio.
    TxMessage oldest{};
    if (xQueueReceive(s_tx_queue, &oldest, 0) == pdTRUE) {
        free(oldest.json);
    }

    if (xQueueSend(s_tx_queue, &message, 0) == pdTRUE) {
        ESP_LOGW(TAG, "TX queue penuh; message audio tertua dibuang");
        return true;
    }

    free(message.json);
    ESP_LOGW(TAG, "TX queue tetap penuh; message audio terbaru dibuang");
    return false;
}

static void websocket_audio_tx_task(void *)
{
    ESP_LOGI(TAG, "Audio TX sender START: depth=%u (~%ums pending)",
             (unsigned)TX_QUEUE_DEPTH,
             (unsigned)(TX_QUEUE_DEPTH * FRAMES_PER_MESSAGE * 20));

    while (s_running) {
        TxMessage message{};
        if (xQueueReceive(s_tx_queue, &message, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (!message.json || message.len == 0) {
            free(message.json);
            continue;
        }

        if (!websocket_is_connected() || !websocket_event_gemini_ready()) {
            free(message.json);
            continue;
        }

        const int64_t send_start_us = esp_timer_get_time();
        const int64_t queue_wait_us = send_start_us - message.queued_at_us;
        const esp_err_t err = websocket_send_text(message.json, message.len);
        const int64_t send_elapsed_us = esp_timer_get_time() - send_start_us;
        const size_t message_len = message.len;
        free(message.json);

        if (queue_wait_us >= SEND_WARN_US) {
            ESP_LOGW(TAG, "Audio TX queue wait=%lld ms json=%uB",
                     (long long)(queue_wait_us / 1000),
                     (unsigned)message_len);
        }
        if (send_elapsed_us >= SEND_WARN_US) {
            ESP_LOGW(TAG, "Audio TX send=%lld ms json=%uB",
                     (long long)(send_elapsed_us / 1000),
                     (unsigned)message_len);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Audio TX gagal: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    tx_queue_flush();
    s_tx_task = nullptr;
    ESP_LOGI(TAG, "Audio TX sender STOP");
    vTaskDelete(nullptr);
}

static void websocket_audio_bridge_task(void *)
{
    static int16_t message_pcm[MESSAGE_SAMPLES];
    static int16_t frame_pcm[FRAME_SAMPLES];
    size_t frames_collected = 0;

    ESP_LOGI(TAG, "Audio uplink bridge START: AudioEngine -> Gemini");

    while (s_running) {
        if (!websocket_is_connected() || !websocket_event_gemini_ready()) {
            frames_collected = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!audio_engine_read_mic_frame(frame_pcm, FRAME_SAMPLES, 100)) {
            continue;
        }

        if (!s_running) break;

        memcpy(message_pcm + (frames_collected * FRAME_SAMPLES),
               frame_pcm,
               FRAME_SAMPLES * sizeof(int16_t));
        ++frames_collected;

        if (frames_collected < FRAMES_PER_MESSAGE) continue;

        char *json = nullptr;
        size_t json_len = 0;
        if (!gemini_protocol_build_realtime_audio(
                message_pcm, MESSAGE_SAMPLES, &json, &json_len)) {
            ESP_LOGE(TAG, "Gagal membuat realtime audio message");
            frames_collected = 0;
            continue;
        }

        if (s_running) {
            (void)tx_queue_push(json, json_len);
        } else {
            free(json);
        }

        frames_collected = 0;
    }

    s_bridge_task = nullptr;
    ESP_LOGI(TAG, "Audio uplink bridge STOP");
    vTaskDelete(nullptr);
}

bool websocket_audio_start(void)
{
    if (s_running) return true;
    if (!audio_engine_conversation_active()) {
        ESP_LOGW(TAG, "AudioEngine conversation belum aktif");
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS);
    while ((s_bridge_task || s_tx_task) &&
           (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (s_bridge_task || s_tx_task) {
        ESP_LOGW(TAG, "Audio bridge task sebelumnya belum selesai");
        return false;
    }

    if (!s_tx_queue) {
        s_tx_queue = xQueueCreateStatic(
            TX_QUEUE_DEPTH,
            sizeof(TxMessage),
            reinterpret_cast<uint8_t *>(s_tx_queue_buffer),
            &s_tx_queue_storage);
        if (!s_tx_queue) {
            ESP_LOGE(TAG, "Gagal membuat audio TX queue");
            return false;
        }
    }

    tx_queue_flush();
    s_running = true;

    if (xTaskCreate(websocket_audio_tx_task,
                    "ws_audio_tx",
                    TX_TASK_STACK,
                    nullptr,
                    TASK_PRIORITY,
                    &s_tx_task) != pdPASS) {
        s_running = false;
        s_tx_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat task ws_audio_tx");
        return false;
    }

    if (xTaskCreate(websocket_audio_bridge_task,
                    "ws_audio",
                    BRIDGE_TASK_STACK,
                    nullptr,
                    TASK_PRIORITY,
                    &s_bridge_task) != pdPASS) {
        s_running = false;
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
