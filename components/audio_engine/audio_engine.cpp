#include "audio_engine.h"
#include "audio_input.h"
#include "audio_output.h"
#include "esp_log.h"
#include <stdint.h>

namespace {
static const char *TAG = "AUDIO_ENGINE";
static constexpr size_t MIC_FRAME_SAMPLES = 320;
static bool s_initialized = false;
static bool s_conversation = false;
static bool s_playback = false;
}

extern "C" bool audio_engine_init(void)
{
    if (s_initialized) return true;
    s_initialized = audio_input_init() && audio_output_init();
    ESP_LOGI(TAG, "Audio Engine %s", s_initialized ? "READY" : "FAILED");
    return s_initialized;
}

extern "C" bool audio_engine_start_conversation(void)
{
    if (!s_initialized) return false;
    if (s_conversation) return true;
    if (!audio_input_start()) return false;
    s_conversation = true;
    ESP_LOGI(TAG, "Conversation input START");
    return true;
}

extern "C" void audio_engine_stop_conversation(void)
{
    if (!s_conversation) return;
    audio_input_stop();
    s_conversation = false;
    ESP_LOGI(TAG, "Conversation input STOP");
}

extern "C" bool audio_engine_conversation_active(void)
{
    return s_conversation;
}

extern "C" bool audio_engine_read_mic_frame(int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    if (!s_conversation || !buffer || samples != MIC_FRAME_SAMPLES) return false;
    return audio_input_read(buffer, samples, timeout_ms);
}

extern "C" size_t audio_engine_mic_frame_samples(void)
{
    return MIC_FRAME_SAMPLES;
}

extern "C" bool audio_engine_start_playback(void)
{
    if (!s_initialized) return false;
    if (s_playback) return true;
    if (!audio_output_start()) return false;
    s_playback = true;
    ESP_LOGI(TAG, "Playback START");
    return true;
}

extern "C" void audio_engine_stop_playback(void)
{
    if (!s_playback) return;
    audio_output_stop();
    s_playback = false;
    ESP_LOGI(TAG, "Playback STOP");
}

extern "C" bool audio_engine_playback_active(void)
{
    return s_playback;
}

extern "C" bool audio_engine_write_speaker_pcm(const int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    if (!s_playback || !buffer || !samples) return false;
    return audio_output_write(buffer, samples, timeout_ms);
}

extern "C" bool audio_engine_drain_playback(void)
{
    if (!s_playback) return true;
    return audio_output_drain();
}

extern "C" void audio_engine_stop(void)
{
    audio_engine_stop_conversation();
    audio_engine_stop_playback();
}

// Wake-word ownership remains in the dedicated wakeword component.
// These compatibility entry points intentionally do not modify wakeword state.
extern "C" bool audio_engine_start_wakeword(void) { return false; }
extern "C" void audio_engine_stop_wakeword(void) {}
extern "C" bool audio_engine_process_wakeword(void) { return false; }
extern "C" bool audio_engine_wakeword_detected(void) { return false; }
extern "C" void audio_engine_clear_wakeword(void) {}
