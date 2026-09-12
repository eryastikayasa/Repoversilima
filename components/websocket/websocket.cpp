#include "websocket.h"

#include <string.h>

#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "WEBSOCKET";

static esp_websocket_client_handle_t s_client = nullptr;
static websocket_callbacks_t s_callbacks = {};
static bool s_connected = false;

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)base;

    auto *event = static_cast<esp_websocket_event_data_t *>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        if (s_callbacks.on_connected) {
            s_callbacks.on_connected(s_callbacks.user_ctx);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGI(TAG, "disconnected");
        if (s_callbacks.on_disconnected) {
            s_callbacks.on_disconnected(s_callbacks.user_ctx);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (!event || !event->data_ptr || event->data_len <= 0) {
            break;
        }

        if (event->op_code == 0x1) {
            if (s_callbacks.on_text) {
                s_callbacks.on_text(
                    static_cast<const char *>(event->data_ptr),
                    static_cast<size_t>(event->data_len),
                    s_callbacks.user_ctx);
            }
        } else if (event->op_code == 0x2) {
            if (s_callbacks.on_binary) {
                s_callbacks.on_binary(
                    static_cast<const uint8_t *>(event->data_ptr),
                    static_cast<size_t>(event->data_len),
                    s_callbacks.user_ctx);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        s_connected = false;
        ESP_LOGE(TAG, "transport error");
        if (s_callbacks.on_error) {
            s_callbacks.on_error(s_callbacks.user_ctx);
        }
        break;

    default:
        break;
    }
}

bool websocket_init(const char *uri, const websocket_callbacks_t *callbacks)
{
    if (!uri || uri[0] == '\0' || s_client) {
        return false;
    }

    memset(&s_callbacks, 0, sizeof(s_callbacks));
    if (callbacks) {
        s_callbacks = *callbacks;
    }

    esp_websocket_client_config_t config = {};
    config.uri = uri;

    s_client = esp_websocket_client_init(&config);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return false;
    }

    esp_err_t err = esp_websocket_register_events(
        s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event registration failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = nullptr;
        return false;
    }

    return true;
}

bool websocket_start(void)
{
    if (!s_client) {
        return false;
    }

    return esp_websocket_client_start(s_client) == ESP_OK;
}

void websocket_stop(void)
{
    if (!s_client) {
        return;
    }

    (void)esp_websocket_client_stop(s_client);
    s_connected = false;
}

bool websocket_is_connected(void)
{
    return s_connected;
}

bool websocket_send_text(const char *data, size_t length)
{
    if (!s_client || !s_connected || !data || length == 0) {
        return false;
    }

    return esp_websocket_client_send_text(
               s_client, data, static_cast<int>(length), portMAX_DELAY) >= 0;
}

bool websocket_send_binary(const uint8_t *data, size_t length)
{
    if (!s_client || !s_connected || !data || length == 0) {
        return false;
    }

    return esp_websocket_client_send_bin(
               s_client, reinterpret_cast<const char *>(data),
               static_cast<int>(length), portMAX_DELAY) >= 0;
}
