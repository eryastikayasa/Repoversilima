#include "websocket_event.h"
#include "websocket_transport.h"
#include "websocket_rx.h"
#include "gemini_protocol.h"
#include "gemini_message.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "WS_EVENT";
static volatile bool s_gemini_ready = false;

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

extern "C" void websocket_event_process_complete_message(const char *json, size_t len)
{
    if (!json || len == 0) return;

    const gemini_message_type_t type = gemini_message_classify(json, len);
    const bool handled = gemini_protocol_process_message(json, len);

    if (type == GEMINI_MESSAGE_SETUP) {
        s_gemini_ready = true;
        ESP_LOGI(TAG, "Gemini setupComplete - audio uplink READY");
    }

    if (!handled) {
        ESP_LOGD(TAG, "Pesan Gemini belum dipetakan (%u byte)", (unsigned)len);
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
            s_gemini_ready = false;
            websocket_rx_reset();
            send_gemini_setup();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            s_gemini_ready = false;
            websocket_rx_reset();
            break;

        case WEBSOCKET_EVENT_DATA:
            // WS callback does transport work only: copy/assemble the frame
            // and defer Gemini JSON parsing to the RX worker.
            if (!websocket_rx_enqueue_data(
                    static_cast<esp_websocket_event_data_t *>(event_data))) {
                ESP_LOGD(TAG, "WS RX fragment tidak diterima");
            }
            break;

        default:
            break;
    }

    ESP_LOGD(TAG, "WebSocket event=%ld", (long)event_id);
}

bool websocket_event_gemini_ready(void)
{
    return s_gemini_ready;
}
