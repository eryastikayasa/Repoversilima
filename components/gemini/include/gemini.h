#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gemini_init(const char *uri);
bool gemini_start(void);
void gemini_stop(void);
bool gemini_is_connected(void);
bool gemini_send_audio(const uint8_t *data, size_t length);
bool gemini_send_text(const char *data, size_t length);

#ifdef __cplusplus
}
#endif
