#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*websocket_connected_cb_t)(void *user_ctx);
typedef void (*websocket_disconnected_cb_t)(void *user_ctx);
typedef void (*websocket_text_cb_t)(const char *data, size_t length, void *user_ctx);
typedef void (*websocket_binary_cb_t)(const uint8_t *data, size_t length, void *user_ctx);
typedef void (*websocket_error_cb_t)(void *user_ctx);

typedef struct {
    websocket_connected_cb_t on_connected;
    websocket_disconnected_cb_t on_disconnected;
    websocket_text_cb_t on_text;
    websocket_binary_cb_t on_binary;
    websocket_error_cb_t on_error;
    void *user_ctx;
} websocket_callbacks_t;

/**
 * Initialize the WebSocket transport.
 * The transport does not interpret audio, JSON, Gemini messages, or protocol state.
 */
bool websocket_init(const char *uri, const websocket_callbacks_t *callbacks);

bool websocket_start(void);
void websocket_stop(void);
bool websocket_is_connected(void);

bool websocket_send_text(const char *data, size_t length);
bool websocket_send_binary(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
