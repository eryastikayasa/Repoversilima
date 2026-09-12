#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_output_init(void);
bool audio_output_start(void);
void audio_output_stop(void);
bool audio_output_write(const int16_t *buffer, size_t samples, uint32_t timeout_ms);
bool audio_output_drain(void);

#ifdef __cplusplus
}
#endif
