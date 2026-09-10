#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t websocket_init(void);
esp_err_t websocket_connect(void);
esp_err_t websocket_disconnect(void);
bool websocket_is_connected(void);
esp_err_t websocket_send_text(const char *text, size_t len);
esp_err_t websocket_send_binary(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
