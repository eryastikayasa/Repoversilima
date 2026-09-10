#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_websocket_client.h"

#ifdef __cplusplus
extern "C" {
#endif

// RX transport is responsible only for WebSocket fragment assembly and
// delivery of complete text messages to the Gemini protocol worker.
bool websocket_rx_init(void);
void websocket_rx_reset(void);
bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data);

#ifdef __cplusplus
}
#endif
