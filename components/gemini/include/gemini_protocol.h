#pragma once
#include <stddef.h>
#include <stdint.h>

bool gemini_protocol_send_audio(const uint8_t *data, size_t length);
bool gemini_protocol_send_text(const char *data, size_t length);
void gemini_protocol_on_text(const char *data, size_t length);
void gemini_protocol_on_binary(const uint8_t *data, size_t length);
