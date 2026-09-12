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

static void tx_queue_flush(void)
{
    if (!s_tx_queue) return;
    TxMessage message{};
    size_t flushed = 0;
    while (xQueueReceive(s_tx_queue, &message, 0) == pdTRUE) {
        free(message.json);
        ++flushed;
    }
    if (flushed) ESP_LOGW(TAG, "TX queue di-flush saat lifecycle abort: %u message", (unsigned)flushed);
}

static bool tx_queue_push(char *json, size_t json_len)
{
    if (!json || json_len == 0 || !s_tx_queue) { free(json); return false; }
    TxMessage message{json, json_len, esp_timer_get_time()};
    while (s_running) {
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
        if (!message.json || message.len == 0) { free(message.json); continue; }
        const bool sent = send_message_until_delivered(&message);
        if (!sent) {
            free(message.json);
            break;
        }
        free(message.json);
    }
    if (!s_draining && !s_running) {
        // Jangan flush: lifecycle baru boleh kehilangan antrean setelah drain gagal ditangani caller.
    }
    s_tx_task = nullptr;
    vTaskDelete(nullptr);
}

static void websocket_audio_bridge_task(void *)
{
    static int16_t message_pcm[MESSAGE_SAMPLES];
    static int16_t frame_pcm[FRAME_SAMPLES];
    size_t frames_collected = 0;
    while (s_running) {
        if (!websocket_is_connected() || !websocket_event_gemini_ready()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!audio_engine_read_mic_frame(frame_pcm, FRAME_SAMPLES, UINT32_MAX)) continue;
        if (!s_running) break;
        memcpy(message_pcm + frames_collected * FRAME_SAMPLES, frame_pcm, FRAME_SAMPLES * sizeof(int16_t));
        ++frames_collected;
        if (frames_collected < FRAMES_PER_MESSAGE) continue;
        char *json = nullptr;
        size_t json_len = 0;
        if (!gemini_protocol_build_realtime_audio(message_pcm, MESSAGE_SAMPLES, &json, &json_len)) {
            ESP_LOGE(TAG, "Gagal membuat realtime audio message; conversation abort");
            s_tx_fatal_error = true;
            s_running = false;
            frames_collected = 0;
            continue;
        }
        ESP_LOGI(TAG, "AudioEngine->TX: %u PCM samples -> %u byte JSON", (unsigned)MESSAGE_SAMPLES, (unsigned)json_len);
        if (!tx_queue_push(json, json_len) && s_running) {
            ESP_LOGE(TAG, "Audio TX queue gagal menerima message; conversation abort");
            s_tx_fatal_error = true;
            s_running = false;
        }
        frames_collected = 0;
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
    tx_queue_flush();
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
    s_running = false;
    const TickType_t bridge_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS);
    while (s_bridge_task && (int32_t)(xTaskGetTickCount() - bridge_deadline) < 0) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_bridge_task) {
        ESP_LOGE(TAG, "TX bridge drain timeout");
        return false;
    }

    s_draining = true;
    const TickType_t tx_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_WAIT_MS + 15000);
    while (s_tx_task && (int32_t)(xTaskGetTickCount() - tx_deadline) < 0) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_tx_task) {
        ESP_LOGE(TAG, "TX queue drain timeout: audio tidak boleh dianggap lengkap");
        return false;
    }
    s_draining = false;
    return !s_tx_fatal_error;
}
