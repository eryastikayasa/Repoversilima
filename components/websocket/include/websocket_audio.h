#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool websocket_audio_start(void);
void websocket_audio_stop(void);
bool websocket_audio_running(void);
bool websocket_audio_drain_stop(void);

#ifdef __cplusplus
}
#endif
