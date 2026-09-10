#pragma once

#include <stddef.h>

#define WEBSOCKET_CONNECT_TIMEOUT_MS 10000
#define WEBSOCKET_RECONNECT_DELAY_MS 3000
#define WEBSOCKET_MAX_TEXT_PAYLOAD 8192
#define WEBSOCKET_MAX_BINARY_PAYLOAD 4096

// Endpoint/API key are resolved at runtime; secrets must not be hard-coded.
