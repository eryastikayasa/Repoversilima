#include "websocket.h"
#include "websocket_transport.h"
#include "websocket_rx.h"

esp_err_t websocket_init(void)
{
    if (!websocket_rx_init()) {
        return ESP_FAIL;
    }

    return websocket_transport_init();
}

esp_err_t websocket_connect(void)
{
    return websocket_transport_connect();
}

esp_err_t websocket_disconnect(void)
{
    return websocket_transport_disconnect();
}

bool websocket_is_connected(void)
{
    return websocket_transport_is_connected();
}

esp_err_t websocket_send_text(const char *text, size_t len)
{
    return websocket_transport_send_text(text, len);
}

esp_err_t websocket_send_binary(const uint8_t *data, size_t len)
{
    return websocket_transport_send_binary(data, len);
}
