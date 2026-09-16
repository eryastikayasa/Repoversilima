#include "websocket_audio.h"
#include "websocket.h"
#include "websocket_event.h"
#include "gemini/gemini_audio.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";

// AudioEngine contract: 20 ms = 320 samples @ 16 kHz PCM16 mono.
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAME_BYTES = FRAME_SAMPLES * sizeof(int16_t);
// Match Repo3 producer contract: 5 x 20 ms = 100 ms / 3200 bytes.
static constexpr size_t FRAMES_PER_BUFFER = 5;
static constexpr size_t TX_BUFFER_BYTES = FRAME_BYTES * FRAMES_PER_BUFFER;
static constexpr uint32_t TASK_STACK = 6144;
static constexpr UBaseType_t TASK_PRIORITY = 5;
static constexpr int32_t SILENCE_THRESHOLD = 500;
static constexpr size_t MIN_ACTIVE_SAMPLES = 8;

static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;

static bool mic_buffer_has_activity(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return false;

    size_t active_samples = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t sample = pcm[i];
        const int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude >= SILENCE_THRESHOLD) {
            ++active_samples;
            if (active_samples >= MIN_ACTIVE_SAMPLES) return true;
        }
    }
    return false;
}

static void websocket_audio_task(void *)
{
    static int16_t tx_pcm[TX_BUFFER_BYTES / sizeof(int16_t)];
    static int16_t frame_pcm[FRAME_SAMPLES];
    size_t frames_collected = 0;

    ESP_LOGI(TAG,
             "Audio uplink START: MIC frame=%u samples/%uB, TX buffer=%uB, central chunk=1600B",
             (unsigned)FRAME_SAMPLES,
             (unsigned)FRAME_BYTES,
             (unsigned)TX_BUFFER_BYTES);

    while (s_running) {
        if (!websocket_is_connected()) {
            frames_collected = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Exactly one blocking point: receive an already-captured 20 ms frame
        // from AudioEngine. No WebSocket/TLS/JSON/Base64 operation happens here.
        if (!audio_engine_read_mic_frame(frame_pcm, FRAME_SAMPLES, 100)) {
            continue;
        }

        memcpy(tx_pcm + (frames_collected * FRAME_SAMPLES),
               frame_pcm,
               FRAME_BYTES);
        ++frames_collected;

        if (frames_collected < FRAMES_PER_BUFFER) continue;

        // Repo3 does not uplink microphone audio while Gemini is speaking.
        // This prevents echo/silence from competing with the central TX worker
        // and preserves the same turn ownership semantics.
        if (gemini_audio_turn_active()) {
            frames_collected = 0;
            continue;
        }

        // Match Repo3's MIC TX gate: do not continuously enqueue silence.
        if (!mic_buffer_has_activity(tx_pcm, TX_BUFFER_BYTES / sizeof(int16_t))) {
            static uint32_t silent_drops = 0;
            ++silent_drops;
            if ((silent_drops & 0x3FU) == 1U) {
                ESP_LOGI(TAG, "MIC TX gate: silent 100ms buffer dropped total=%lu",
                         (unsigned long)silent_drops);
            }
            frames_collected = 0;
            continue;
        }

        const bool queued = websocket_tx_enqueue_audio(
            reinterpret_cast<const uint8_t *>(tx_pcm),
            TX_BUFFER_BYTES);
        if (!queued) {
            ESP_LOGW(TAG, "Audio TX buffer gagal di-enqueue; MIC tetap nonblocking");
        }

        frames_collected = 0;
    }

    s_task = nullptr;
    ESP_LOGI(TAG, "Audio uplink STOP");
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
    const BaseType_t result = xTaskCreate(
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
