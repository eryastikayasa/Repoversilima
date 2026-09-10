#include "websocket_event.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "WS_EVENT";

static void send_gemini_setup(void)
{
    char *setup = nullptr;
    size_t setup_len = 0;

    if (!gemini_protocol_build_setup(&setup, &setup_len)) {
        ESP_LOGE(TAG, "Gagal membuat Gemini setup");
        return;
    }

    const esp_err_t err = websocket_transport_send_text(setup, setup_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Gemini setup terkirim (%u byte)", (unsigned)setup_len);
    } else {
        ESP_LOGE(TAG, "Gagal mengirim Gemini setup: %s", esp_err_to_name(err));
    }

    free(setup);
}

static void handle_websocket_data(void *event_data)
{
    if (!event_data) return;

    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);

    if (!data->data_ptr || data->data_len == 0) return;

    // Untuk checkpoint ini kita hanya menerima JSON yang datang sebagai satu
    // frame lengkap. Fragment reassembly akan dibuat pada tahap berikutnya.
    if (data->payload_offset != 0 || data->payload_offset + data->data_len != data->payload_len) {
        ESP_LOGW(TAG, "Gemini message terfragmentasi: offset=%u data=%u total=%u",
                 (unsigned)data->payload_offset,
                 (unsigned)data->data_len,
                 (unsigned)data->payload_len);
        return;
    }

    if (data->op_code != 0x1) {
        ESP_LOGD(TAG, "WebSocket data non-text opcode=0x%02X", data->op_code);
        return;
    }

    const bool handled = gemini_protocol_process_message(
        static_cast<const char *>(data->data_ptr), data->data_len);

    if (!handled) {
        ESP_LOGD(TAG, "Pesan Gemini belum dipetakan (%u byte)",
                 (unsigned)data->data_len);
    }
}

void websocket_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data)
{
    (void)handler_args;
    (void)base;

    websocket_transport_handle_event(event_id, event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            send_gemini_setup();
            break;

        case WEBSOCKET_EVENT_DATA:
            handle_websocket_data(event_data);
            break;

        default:
            break;
    }

    ESP_LOGD(TAG, "WebSocket event=%ld", (long)event_id);
}
