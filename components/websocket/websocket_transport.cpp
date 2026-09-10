#include "websocket_transport.h"

esp_err_t websocket_transport_init(void)
{
    return ESP_OK;
}

esp_err_t websocket_transport_connect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t websocket_transport_disconnect(void)
{
    return ESP_OK;
}

bool websocket_transport_is_connected(void)
{
    return false;
}

esp_err_t websocket_transport_send_text(const char *text, size_t len)
{
    (void)text;
    (void)len;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t websocket_transport_send_binary(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_INVALID_STATE;
}
