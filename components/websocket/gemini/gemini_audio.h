#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gemini_audio_process_server_message(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
