#include "websocket.h"
#include "websocket_transport.h"
#include "websocket_event.h"

esp_err_t websocket_init(void)
{
    const esp_err_t err = websocket_transport_init();
    if (err != ESP_OK) {
        return err;
    }

    if (!websocket_event_init()) {
        return ESP_FAIL;
    }

    return ESP_OK;
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
