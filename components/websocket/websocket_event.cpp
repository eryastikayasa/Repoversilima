#include "websocket_event.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "gemini_message.h"
#include "gemini_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_EVENT";
static volatile bool s_gemini_ready = false;

// Gemini Live responses can be larger than the WebSocket client's internal
// event chunks. Complete-message storage lives in PSRAM so the event callback
// does not consume a large DRAM/static buffer.
static constexpr size_t RX_MAX_PAYLOAD = 64 * 1024;
static constexpr size_t RX_DIAGNOSTIC_MAX = 512;
static constexpr size_t RX_BUFFER_COUNT = 6;
static constexpr uint32_t RX_WORKER_STACK = 8192;
static constexpr UBaseType_t RX_WORKER_PRIORITY = 5;

static QueueHandle_t s_rx_free_queue = nullptr;
static QueueHandle_t s_rx_ready_queue = nullptr;
static StaticQueue_t s_rx_free_queue_storage;
static StaticQueue_t s_rx_ready_queue_storage;
static char *s_rx_free_storage[RX_BUFFER_COUNT];
static char *s_rx_ready_storage[RX_BUFFER_COUNT];
static char *s_rx_buffers[RX_BUFFER_COUNT] = {};
static TaskHandle_t s_rx_worker_task = nullptr;
static bool s_rx_worker_ready = false;

static char *s_rx_assembling_buffer = nullptr;
static size_t s_rx_expected = 0;
static size_t s_rx_received = 0;
static bool s_rx_assembling = false;

static bool ensure_rx_worker(void)
{
    if (s_rx_worker_ready) return true;

    s_rx_free_queue = xQueueCreateStatic(
        RX_BUFFER_COUNT,
        sizeof(char *),
        reinterpret_cast<uint8_t *>(s_rx_free_storage),
        &s_rx_free_queue_storage);
    s_rx_ready_queue = xQueueCreateStatic(
        RX_BUFFER_COUNT,
        sizeof(char *),
        reinterpret_cast<uint8_t *>(s_rx_ready_storage),
        &s_rx_ready_queue_storage);
    if (!s_rx_free_queue || !s_rx_ready_queue) {
        ESP_LOGE(TAG, "Gagal membuat RX queue");
        return false;
    }

    for (size_t i = 0; i < RX_BUFFER_COUNT; ++i) {
        s_rx_buffers[i] = static_cast<char *>(heap_caps_malloc(
            RX_MAX_PAYLOAD + 1,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_rx_buffers[i]) {
            ESP_LOGE(TAG, "Gagal alokasi RX buffer PSRAM #%u (%u byte)",
                     (unsigned)i,
                     (unsigned)(RX_MAX_PAYLOAD + 1));
            return false;
        }
        if (xQueueSend(s_rx_free_queue, &s_rx_buffers[i], 0) != pdPASS) {
            ESP_LOGE(TAG, "Gagal memasukkan RX buffer #%u ke free queue", (unsigned)i);
            return false;
        }
    }

    BaseType_t result = xTaskCreate(
        [](void *) {
            ESP_LOGI(TAG, "Gemini RX worker START: buffers=%u x %uKB PSRAM",
                     (unsigned)RX_BUFFER_COUNT,
                     (unsigned)(RX_MAX_PAYLOAD / 1024));

            while (true) {
                char *json = nullptr;
                if (xQueueReceive(s_rx_ready_queue, &json, portMAX_DELAY) != pdPASS) {
                    continue;
                }

                if (!json) continue;

                const size_t len = strlen(json);
                ESP_LOGI(TAG, "Gemini RX worker process: %u byte", (unsigned)len);

                const gemini_message_type_t type = gemini_message_classify(json, len);
                const bool protocol_handled = gemini_protocol_process_message(json, len);
                const bool audio_handled = gemini_audio_process_server_message(json, len);
                const bool handled = protocol_handled || audio_handled;

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

                if (xQueueSend(s_rx_free_queue, &json, portMAX_DELAY) != pdPASS) {
                    ESP_LOGE(TAG, "Gagal mengembalikan RX buffer ke free queue");
                }
            }
        },
        "ws_rx",
        RX_WORKER_STACK,
        nullptr,
        RX_WORKER_PRIORITY,
        &s_rx_worker_task);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat task ws_rx");
        return false;
    }

    s_rx_worker_ready = true;
    return true;
}

static void reset_rx(void)
{
    if (s_rx_assembling_buffer) {
        if (s_rx_free_queue) {
            if (xQueueSend(s_rx_free_queue, &s_rx_assembling_buffer, 0) != pdPASS) {
                ESP_LOGW(TAG, "RX buffer assembly gagal dikembalikan");
            }
        }
        s_rx_assembling_buffer = nullptr;
    }
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_assembling = false;
}

static void handle_data_event(esp_websocket_event_data_t *data)
{
    if (!data || !data->data_ptr || data->data_len <= 0 || data->payload_len <= 0) {
        return;
    }

    const size_t payload_len = (size_t)data->payload_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t chunk_len = (size_t)data->data_len;

    if (payload_len > RX_MAX_PAYLOAD ||
        offset > payload_len ||
        chunk_len > payload_len - offset) {
        ESP_LOGW(TAG, "RX payload tidak valid: total=%u max=%u",
                 (unsigned)payload_len,
                 (unsigned)RX_MAX_PAYLOAD);
        reset_rx();
        return;
    }

    if (!ensure_rx_worker()) {
        ESP_LOGE(TAG, "RX worker belum siap; payload dibuang");
        reset_rx();
        return;
    }

    if (offset == 0) {
        reset_rx();

        if (xQueueReceive(s_rx_free_queue, &s_rx_assembling_buffer, 0) != pdPASS) {
            ESP_LOGW(TAG, "RX buffer penuh; payload Gemini dibuang (%u byte)",
                     (unsigned)payload_len);
            return;
        }

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

    memcpy(s_rx_assembling_buffer + offset, data->data_ptr, chunk_len);
    s_rx_received += chunk_len;

    if (s_rx_received != s_rx_expected) return;

    s_rx_assembling_buffer[s_rx_expected] = '\0';
    char *ready_buffer = s_rx_assembling_buffer;
    s_rx_assembling_buffer = nullptr;
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_assembling = false;

    if (xQueueSend(s_rx_ready_queue, &ready_buffer, 0) != pdPASS) {
        ESP_LOGW(TAG, "RX ready queue penuh; payload Gemini dibuang");
        if (xQueueSend(s_rx_free_queue, &ready_buffer, 0) != pdPASS) {
            ESP_LOGE(TAG, "RX buffer hilang setelah ready queue penuh");
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
            ensure_rx_worker();
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
