#include "audio_input.h"
#include "audio_hal.h"

bool audio_input_init(void)
{
    audio_hal_init();
    return true;
}

bool audio_input_start(void)
{
    return audio_hal_start_capture() == ESP_OK;
}

void audio_input_stop(void)
{
    (void)audio_hal_stop_capture();
}

bool audio_input_read(int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!buffer || !samples) return false;
    size_t read = 0;
    return audio_hal_read_pcm(buffer, samples, &read) == ESP_OK && read == samples;
}
