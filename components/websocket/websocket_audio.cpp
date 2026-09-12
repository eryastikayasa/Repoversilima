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
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAMES_PER_MESSAGE = 5;
static constexpr size_t MESSAGE_SAMPLES = FRAME_SAMPLES * FRAMES_PER_MESSAGE;
static constexpr size_t TX_QUEUE_DEPTH = 40;
static constexpr uint32_t BRIDGE_TASK_STACK = 4096;
static constexpr uint32_t TX_TASK_STACK = 4096;
static constexpr UBaseType_t TASK_PRIORITY = 5;
static constexpr uint32_t STOP_WAIT_MS = 15000;

struct TxMessage { char *json; size_t len; int64_t queued_at_us; };
static StaticQueue_t s_tx_queue_storage;
static TxMessage s_tx_queue_buffer[TX_QUEUE_DEPTH];
static QueueHandle_t s_tx_queue = nullptr;
static TaskHandle_t s_bridge_task = nullptr;
static TaskHandle_t s_tx_task = nullptr;
static volatile bool s_running = false;
static volatile bool s_draining = false;
static volatile bool s_tx_fatal_error = false;
static uint32_t s_tx_message_count = 0;

static bool tx_queue_push_blocking(char *json, size_t json_len)
{
    if (!json || json_len == 0 || !s_tx_queue) { free(json); return false; }
    TxMessage message{json, json_len, esp_timer_get_time()};
    while (s_running || s_draining) {
        if (xQueueSend(s_tx_queue, &message, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "MIC->TX: message #%u queued, json=%u byte, queue=%u/%u",
                     (unsigned)(s_tx_message_count + 1), (unsigned)json_len,
                     (unsigned)uxQueueMessagesWaiting(s_tx_queue), (unsigned)TX_QUEUE_DEPTH);
            return true;
        }
    }
    free(message.json);
    return false;
}

static bool send_message_until_delivered(TxMessage *message)
{
    if (!message || !message->json || message->len == 0) return false;
    while (s_running || s_draining) {
        while (s_running || s_draining) {
            if (websocket_is_connected() && websocket_event_gemini_ready()) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!(s_running || s_draining)) return false;
        const esp_err_t err = websocket_send_text(message->json, message->len);
        if (err == ESP_OK) {
            ++s_tx_message_count;
            ESP_LOGI(TAG, "TX->Gemini: message #%u sent, json=%u byte, queue=%u/%u",
                     (unsigned)s_tx_message_count, (unsigned)message->len,
                     (unsigned)uxQueueMessagesWaiting(s_tx_queue), (unsigned)TX_QUEUE_DEPTH);
            websocket_event_note_activity();
            return true;
        }
        ESP_LOGW(TAG, "Audio TX gagal, menunggu lalu kirim ulang: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

static void websocket_audio_tx_task(void *)
{
    while (s_running || s_draining) {
        TxMessage message{};
        if (xQueueReceive(s_tx_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (!message.json || message.len == 0) continue;
        if (!send_message_until_delivered(&message)) {
            s_tx_fatal_error = true;
            free(message.json);
            break;
        }
        free(message.json);
    }
    s_tx_task = nullptr;
    vTaskDelete(nullptr);
}

static void websocket_audio_bridge_task(void *)
{
    static int16_t message_pcm[MESSAGE_SAMPLES];
    static int16_t frame_pcm[FRAME_SAMPLES];
    size_t frames_collected = 0;

    while (s_running || s_draining) {
        if (!websocket_is_connected() || !websocket_event_gemini_ready()) {
            if (s_draining) {
                // During drain, do not discard already captured MIC frames merely because
                // the transport is temporarily unavailable. Read them first, then queue.
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        if (!audio_engine_read_mic_frame(frame_pcm, FRAME_SAMPLES, 100)) {
            if (s_draining) break;
            continue;
        }
        memcpy(message_pcm + frames_collected * FRAME_SAMPLES, frame_pcm, FRAME_SAMPLES * sizeof(int16_t));
        ++frames_collected;
        if (frames_collected < FRAMES_PER_MESSAGE) continue;

        char *json = nullptr;
        size_t json_len = 0;
        if (!gemini_protocol_build_realtime_audio(message_pcm, MESSAGE_SAMPLES, &json, &json_len)) {
            ESP_LOGE(TAG, "Gagal membuat realtime audio message");
            s_tx_fatal_error = true;
            s_running = false;
            break;
        }
        ESP_LOGI(TAG, "AudioEngine->TX: %u PCM samples -> %u byte JSON", (unsigned)MESSAGE_SAMPLES, (unsigned)json_len);
        if (!tx_queue_push_blocking(json, json_len)) {
            s_tx_fatal_error = true;
            break;
        }
        frames_collected = 0;
    }

    // Preserve all frames already captured before stop. A final 1..4 frame batch is
    // sent with its exact sample count; no zero padding and no samples are discarded.
    if (frames_collected > 0 && !s_tx_fatal_error && s_tx_queue) {
        const size_t final_samples = frames_collected * FRAME_SAMPLES;
        char *json = nullptr;
        size_t json_len = 0;
        if (gemini_protocol_build_realtime_audio(message_pcm, final_samples, &json, &json_len)) {
            ESP_LOGI(TAG, "AudioEngine->TX: final %u PCM samples -> %u byte JSON", (unsigned)final_samples, (unsigned)json_len);
            if (!tx_queue_push_blocking(json, json_len)) s_tx_fatal_error = true;
        } else {
            s_tx_fatal_error = true;
        }
    }

    s_bridge_task = nullptr;
    vTaskDelete(nullptr);
}

bool websocket_audio_start(void)
{
    if (s_running) return true;
    if (!audio_engine_conversation_active()) return false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS);
    while ((s_bridge_task || s_tx_task) && (int32_t)(xTaskGetTickCount() - deadline) < 0) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_bridge_task || s_tx_task) return false;
    if (!s_tx_queue) {
        s_tx_queue = xQueueCreateStatic(TX_QUEUE_DEPTH, sizeof(TxMessage), reinterpret_cast<uint8_t *>(s_tx_queue_buffer), &s_tx_queue_storage);
        if (!s_tx_queue) return false;
    }
    if (uxQueueMessagesWaiting(s_tx_queue) != 0) {
        ESP_LOGE(TAG, "TX queue masih berisi data; lifecycle sebelumnya belum drain");
        return false;
    }
    s_tx_fatal_error = false;
    s_tx_message_count = 0;
    s_draining = false;
    s_running = true;
    websocket_event_note_activity();
    if (xTaskCreate(websocket_audio_tx_task, "ws_audio_tx", TX_TASK_STACK, nullptr, TASK_PRIORITY, &s_tx_task) != pdPASS) {
        s_running = false;
        s_tx_task = nullptr;
        return false;
    }
    if (xTaskCreate(websocket_audio_bridge_task, "ws_audio", BRIDGE_TASK_STACK, nullptr, TASK_PRIORITY, &s_bridge_task) != pdPASS) {
        s_running = false;
        s_draining = true;
        return false;
    }
    return true;
}

void websocket_audio_stop(void) { s_running = false; }
bool websocket_audio_running(void) { return s_running; }

bool websocket_audio_drain_stop(void)
{
    // Drain mode is enabled before stopping the bridge. The bridge first consumes every
    // already-captured MIC frame, then emits the exact final partial batch.
    s_draining = true;
    s_running = false;

    const TickType_t bridge_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS);
    while (s_bridge_task && (int32_t)(xTaskGetTickCount() - bridge_deadline) < 0) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_bridge_task) {
        ESP_LOGE(TAG, "TX bridge drain timeout: data belum boleh dibuang");
        return false;
    }

    const TickType_t tx_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS + 15000);
    while (s_tx_task && (int32_t)(xTaskGetTickCount() - tx_deadline) < 0) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_tx_task) {
        ESP_LOGE(TAG, "TX queue drain timeout: audio belum lengkap dan tidak dianggap selesai");
        return false;
    }
    s_draining = false;
    return !s_tx_fatal_error && uxQueueMessagesWaiting(s_tx_queue) == 0;
}
