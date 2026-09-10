#include "websocket_audio.h"
#include "websocket.h"
#include "websocket_event.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";

// AudioEngine conversation contract: 20 ms = 320 samples @ 16 kHz PCM16 mono.
// Keep this contract identical on both sides; changing only the WS side can
// make xQueueReceive copy 640 bytes into a smaller destination buffer.
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAMES_PER_MESSAGE = 5;
static constexpr size_t MESSAGE_SAMPLES = FRAME_SAMPLES * FRAMES_PER_MESSAGE;
static constexpr size_t MESSAGE_BYTES = MESSAGE_SAMPLES * sizeof(int16_t);
static constexpr uint32_t TASK_STACK = 6144;
static constexpr UBaseType_t TASK_PRIORITY = 5;

static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;

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

        const esp_err_t err = websocket_send_text(json, json_len);
        free(json);
        frames_collected = 0;

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Audio uplink gagal: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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
