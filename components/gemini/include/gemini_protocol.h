#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gemini_protocol_init(void);
void gemini_protocol_reset(void);
void gemini_protocol_on_connected(void);

bool gemini_protocol_send_audio(const uint8_t *data, size_t length);
bool gemini_protocol_send_text(const char *data, size_t length);

void gemini_protocol_on_text(const char *data, size_t length);
void gemini_protocol_on_binary(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
