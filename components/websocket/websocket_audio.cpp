#include "websocket_audio.h"
#include "websocket.h"
#include "websocket_event.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";

// AudioEngine conversation contract: 20 ms = 320 samples @ 16 kHz PCM16 mono.
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAMES_PER_MESSAGE = 5;
static constexpr size_t MESSAGE_SAMPLES = FRAME_SAMPLES * FRAMES_PER_MESSAGE;
static constexpr size_t MESSAGE_BYTES = MESSAGE_SAMPLES * sizeof(int16_t);

// The capture task must never wait on the network. Six messages = 600 ms of
// audio backlog; when full, discard the oldest queued message and keep the
// newest capture data so latency does not grow without bound.
static constexpr size_t TX_QUEUE_DEPTH = 6;
static constexpr uint32_t CAPTURE_TASK_STACK = 6144;
static constexpr uint32_t TX_TASK_STACK = 4096;
static constexpr UBaseType_t TASK_PRIORITY = 5;
static constexpr int64_t SEND_WARN_US = 80000;
static constexpr uint32_t STOP_WAIT_MS = 300;
static constexpr uint32_t TX_DIAGNOSTIC_EVERY = 10;

struct TxMessage {
    char *json;
    size_t len;
    int64_t queued_at_us;
};

static StaticQueue_t s_tx_queue_storage;
static TxMessage s_tx_queue_buffer[TX_QUEUE_DEPTH];
static QueueHandle_t s_tx_queue = nullptr;

static TaskHandle_t s_capture_task = nullptr;
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

    // Queue is full. Drop the oldest audio message, never block the capture
    // path waiting for a slow network send.
    TxMessage oldest{};
    if (xQueueReceive(s_tx_queue, &oldest, 0) == pdTRUE) {
        free(oldest.json);
    }

    if (xQueueSend(s_tx_queue, &message, 0) != pdTRUE) {
        free(message.json);
        ESP_LOGW(TAG, "TX queue tetap penuh; audio message terbaru drop");
        return false;
    }

    ESP_LOGW(TAG, "TX queue penuh; audio message tertua drop, terbaru dipertahankan");
    return true;
}

static void log_tx_diagnostic(uint32_t sent_count,
                              int64_t queue_wait_us,
                              int64_t send_elapsed_us)
{
    if (!s_tx_queue || (sent_count % TX_DIAGNOSTIC_EVERY) != 0) return;

    const UBaseType_t queued = uxQueueMessagesWaiting(s_tx_queue);
    const UBaseType_t free_stack = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t cpu_hz = (uint32_t)esp_clk_cpu_freq();
    const size_t free_heap = esp_get_free_heap_size();
    const size_t min_heap = esp_get_minimum_free_heap_size();

    ESP_LOGI(TAG,
             "TX DIAG: count=%u queue_wait=%lldms send=%lldms queue=%u/%u stack_free=%u heap=%u minheap=%u cpu=%uMHz",
             (unsigned)sent_count,
             (long long)(queue_wait_us / 1000),
             (long long)(send_elapsed_us / 1000),
             (unsigned)queued,
             (unsigned)TX_QUEUE_DEPTH,
             (unsigned)free_stack,
             (unsigned)free_heap,
             (unsigned)min_heap,
             (unsigned)(cpu_hz / 1000000U));

    wifi_log_diagnostic();
}

static void websocket_audio_tx_task(void *)
{
    ESP_LOGI(TAG, "Audio TX sender START: queue=%u messages (~%ums)",
             (unsigned)TX_QUEUE_DEPTH,
             (unsigned)(TX_QUEUE_DEPTH * FRAMES_PER_MESSAGE * 20));

    uint32_t sent_count = 0;

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
        ++sent_count;

        if (queue_wait_us >= SEND_WARN_US) {
            ESP_LOGW(TAG, "Audio TX menunggu queue lama: %lld ms, json=%uB",
                     (long long)(queue_wait_us / 1000),
                     (unsigned)message_len);
        }

        if (send_elapsed_us >= SEND_WARN_US) {
            ESP_LOGW(TAG, "Audio TX send lambat: %lld ms, json=%uB",
                     (long long)(send_elapsed_us / 1000),
                     (unsigned)message_len);
        }

        log_tx_diagnostic(sent_count, queue_wait_us, send_elapsed_us);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Audio TX gagal: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    const TickType_t wait_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS);
    while (s_capture_task != nullptr &&
           (int32_t)(xTaskGetTickCount() - wait_deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    tx_queue_flush();

    s_tx_task = nullptr;
    ESP_LOGI(TAG, "Audio TX sender STOP");
    vTaskDelete(nullptr);
}

static void websocket_audio_capture_task(void *)
{
    static int16_t message_pcm[MESSAGE_SAMPLES];
    static int16_t frame_pcm[FRAME_SAMPLES];
    size_t frames_collected = 0;

    ESP_LOGI(TAG,
             "Audio uplink capture START: frame=%u samples/%uB, message=%u samples/%uB",
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

        if (!s_running) {
            break;
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

        if (!s_running) {
            free(json);
            break;
        }

        if (!tx_queue_push(json, json_len)) {
            ESP_LOGW(TAG, "Audio uplink message drop sebelum TX");
        }

        frames_collected = 0;
    }

    s_capture_task = nullptr;
    ESP_LOGI(TAG, "Audio uplink capture STOP");
    vTaskDelete(nullptr);
}

bool websocket_audio_start(void)
{
    if (s_running) return true;
    if (!audio_engine_conversation_active()) {
        ESP_LOGW(TAG, "Conversation AudioEngine belum aktif");
        return false;
    }

    const TickType_t wait_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS);
    while ((s_capture_task != nullptr || s_tx_task != nullptr) &&
           (int32_t)(xTaskGetTickCount() - wait_deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (s_capture_task != nullptr || s_tx_task != nullptr) {
        ESP_LOGW(TAG, "Audio uplink task sebelumnya belum selesai");
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

    BaseType_t tx_result = xTaskCreate(
        websocket_audio_tx_task,
        "ws_audio_tx",
        TX_TASK_STACK,
        nullptr,
        TASK_PRIORITY,
        &s_tx_task);

    if (tx_result != pdPASS) {
        s_running = false;
        s_tx_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat task ws_audio_tx");
        return false;
    }

    BaseType_t capture_result = xTaskCreate(
        websocket_audio_capture_task,
        "ws_audio",
        CAPTURE_TASK_STACK,
        nullptr,
        TASK_PRIORITY,
        &s_capture_task);

    if (capture_result != pdPASS) {
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
