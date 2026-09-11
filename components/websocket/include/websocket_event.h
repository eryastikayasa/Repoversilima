#pragma once

#include "esp_websocket_client.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool websocket_event_init(void);

void websocket_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data);

bool websocket_event_gemini_ready(void);
void websocket_event_note_activity(void);
int64_t websocket_event_last_activity_us(void);
bool websocket_event_drain(void);

#ifdef __cplusplus
}
#endif
