#include "websocket_event.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "gemini_message.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_EVENT";
static volatile bool s_gemini_ready = false;

// Keep RX handling inside the existing event layer. No RX worker/task/queue.
static constexpr size_t RX_MAX_PAYLOAD = 16 * 1024;
static constexpr size_t RX_DIAGNOSTIC_MAX = 512;
static char s_rx_buffer[RX_MAX_PAYLOAD + 1];
static size_t s_rx_expected = 0;
static size_t s_rx_received = 0;
static bool s_rx_assembling = false;

static void reset_rx(void)
{
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_assembling = false;
}

static void process_complete_message(const char *json, size_t len)
{
    if (!json || len == 0) return;

    ESP_LOGI(TAG, "Gemini RX complete: %u byte", (unsigned)len);

    const gemini_message_type_t type = gemini_message_classify(json, len);
    const bool handled = gemini_protocol_process_message(json, len);

    if (type == GEMINI_MESSAGE_SETUP) {
        s_gemini_ready = true;
        ESP_LOGI(TAG, "Gemini setupComplete - audio uplink READY");
    }

    if (!handled) {
        const size_t log_len = len < RX_DIAGNOSTIC_MAX ? len : RX_DIAGNOSTIC_MAX;
        ESP_LOGW(TAG, "Gemini RX belum dipetakan (%u byte)", (unsigned)len);
        ESP_LOGW(TAG, "Gemini RX RAW: %.*s", (int)log_len, json);
        if (len > RX_DIAGNOSTIC_MAX) {
            ESP_LOGW(TAG, "Gemini RX RAW dipotong pada %u byte", (unsigned)RX_DIAGNOSTIC_MAX);
        }
    }
}

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

static void handle_data_event(esp_websocket_event_data_t *data)
{
    if (!data || !data->data_ptr || data->data_len <= 0 || data->payload_len <= 0) {
        return;
    }

    const size_t payload_len = (size_t)data->payload_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t chunk_len = (size_t)data->data_len;

    ESP_LOGI(TAG,
             "Gemini RX chunk: opcode=0x%02X offset=%u len=%u total=%u",
             data->op_code,
             (unsigned)offset,
             (unsigned)chunk_len,
             (unsigned)payload_len);

    if (payload_len > RX_MAX_PAYLOAD ||
        offset > payload_len ||
        chunk_len > payload_len - offset) {
        ESP_LOGW(TAG, "RX payload tidak valid");
        reset_rx();
        return;
    }

    // ESP WebSocket Client reports a fragmented payload through successive
    // DATA events using payload_offset. Treat offset==0 as a new payload and
    // assemble by byte offset; do not require continuation opcode 0x00 because
    // the transport event contract is payload-oriented rather than a raw
    // WebSocket frame parser.
    if (offset == 0) {
        reset_rx();
        s_rx_expected = payload_len;
        s_rx_assembling = true;
    } else if (!s_rx_assembling ||
               s_rx_expected != payload_len ||
               offset != s_rx_received) {
        ESP_LOGW(TAG,
                 "RX fragment sequence tidak valid: offset=%u received=%u expected=%u total=%u",
                 (unsigned)offset,
                 (unsigned)s_rx_received,
                 (unsigned)s_rx_expected,
                 (unsigned)payload_len);
        reset_rx();
        return;
    }

    memcpy(s_rx_buffer + offset, data->data_ptr, chunk_len);
    s_rx_received += chunk_len;

    if (s_rx_received != s_rx_expected) {
        return;
    }

    s_rx_buffer[s_rx_expected] = '\0';
    process_complete_message(s_rx_buffer, s_rx_expected);
    reset_rx();
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
            reset_rx();
            send_gemini_setup();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            s_gemini_ready = false;
            reset_rx();
            break;

        case WEBSOCKET_EVENT_DATA:
            handle_data_event(
                static_cast<esp_websocket_event_data_t *>(event_data));
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
