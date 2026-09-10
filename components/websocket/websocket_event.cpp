#include "websocket_event.h"
#include "websocket_transport.h"
#include "esp_log.h"

static const char *TAG = "WS_EVENT";

void websocket_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data)
{
    (void)handler_args;
    (void)base;

    websocket_transport_handle_event(event_id, event_data);
    ESP_LOGD(TAG, "WebSocket event=%ld", (long)event_id);
}
