#pragma once

#include "esp_websocket_client.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void websocket_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data);

bool websocket_event_gemini_ready(void);

// Called by the RX protocol worker after a complete WebSocket message has
// been assembled. JSON/protocol work never runs inside the WS callback.
void websocket_event_process_complete_message(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
