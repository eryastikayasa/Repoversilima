#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_input_init(void);
bool audio_input_start(void);
void audio_input_stop(void);
bool audio_input_read(int16_t *buffer, size_t samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
