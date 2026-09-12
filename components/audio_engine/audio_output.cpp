#include "audio_output.h"
#include "audio_hal.h"

bool audio_output_init(void)
{
    return true;
}

bool audio_output_start(void)
{
    return audio_hal_start_playback() == ESP_OK;
}

void audio_output_stop(void)
{
    (void)audio_hal_stop_playback();
}

bool audio_output_write(const int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!buffer || !samples) return false;
    size_t written = 0;
    return audio_hal_write_pcm(buffer, samples, &written) == ESP_OK && written == samples;
}

bool audio_output_drain(void)
{
    return true;
}
