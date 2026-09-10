#include "websocket_rx.h"

#include "gemini_message.h"
#include "gemini_protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_RX";

static constexpr size_t MAX_PAYLOAD = 16 * 1024;
static constexpr size_t ASSEMBLY_CAPACITY = MAX_PAYLOAD + 1;
static constexpr size_t QUEUE_DEPTH = 4;
static constexpr uint32_t RX_TASK_STACK = 8192;
static constexpr UBaseType_t RX_TASK_PRIORITY = 4;

struct rx_message_t {
    char *buffer;
    size_t len;
};

static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_task = nullptr;
static uint8_t *s_assembly = nullptr;
static size_t s_expected = 0;
static size_t s_received = 0;
static bool s_assembling = false;
static bool s_initialized = false;

static void reset_assembly(void)
{
    s_expected = 0;
    s_received = 0;
    s_assembling = false;
}

static void free_queued_messages(void)
{
    if (!s_queue) return;

    rx_message_t msg = {};
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
        free(msg.buffer);
        msg = {};
    }
}

static void rx_protocol_task(void *)
{
    rx_message_t msg = {};
    ESP_LOGI(TAG, "Gemini RX worker START - protocol dipisahkan dari WS callback");

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!msg.buffer || msg.len == 0) {
            free(msg.buffer);
            msg = {};
            continue;
        }

        const int64_t start_us = esp_timer_get_time();
        const gemini_message_type_t type = gemini_message_classify(msg.buffer, msg.len);
        const bool handled = gemini_protocol_process_message(msg.buffer, msg.len);
        const uint32_t process_ms =
            (uint32_t)((esp_timer_get_time() - start_us) / 1000);

        if (type == GEMINI_MESSAGE_SETUP) {
            // The event adapter owns the public readiness state. The worker
            // reports the classification here; readiness is updated by the
            // event adapter after a complete message is delivered.
            ESP_LOGI(TAG, "Gemini setupComplete diterima (%u byte, %u ms)",
                     (unsigned)msg.len, (unsigned)process_ms);
        } else {
            ESP_LOGD(TAG, "Gemini RX message type=%d len=%u handled=%d time=%ums",
                     (int)type, (unsigned)msg.len, handled ? 1 : 0,
                     (unsigned)process_ms);
        }

        free(msg.buffer);
        msg = {};
    }
}

bool websocket_rx_init(void)
{
    if (s_initialized) return true;

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(rx_message_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Gagal membuat Gemini RX queue");
        return false;
    }

    s_assembly = static_cast<uint8_t *>(heap_caps_malloc(
        ASSEMBLY_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_assembly) {
        s_assembly = static_cast<uint8_t *>(heap_caps_malloc(
            ASSEMBLY_CAPACITY, MALLOC_CAP_8BIT));
    }
    if (!s_assembly) {
        ESP_LOGE(TAG, "Gagal alokasi RX assembly buffer %u byte",
                 (unsigned)ASSEMBLY_CAPACITY);
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return false;
    }

    reset_assembly();

    if (xTaskCreate(rx_protocol_task, "gemini_rx", RX_TASK_STACK, nullptr,
                    RX_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat Gemini RX worker");
        heap_caps_free(s_assembly);
        s_assembly = nullptr;
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "RX transport siap: max_payload=%u queue=%u",
             (unsigned)MAX_PAYLOAD, (unsigned)QUEUE_DEPTH);
    return true;
}

void websocket_rx_reset(void)
{
    reset_assembly();
    free_queued_messages();
}

bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data)
{
    if (!s_initialized || !s_queue || !s_assembly || !data ||
        !data->data_ptr || data->data_len <= 0 || data->payload_len <= 0) {
        return false;
    }

    const size_t payload_len = (size_t)data->payload_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t chunk_len = (size_t)data->data_len;

    if (payload_len > MAX_PAYLOAD || offset > payload_len ||
        chunk_len > payload_len - offset) {
        ESP_LOGW(TAG, "RX payload ditolak: offset=%u len=%u total=%u",
                 (unsigned)offset, (unsigned)chunk_len, (unsigned)payload_len);
        reset_assembly();
        return false;
    }

    // Only text messages are part of the Gemini JSON control/protocol path.
    // 0x00 is a continuation frame; 0x01 is a text frame.
    if (offset == 0) {
        if (data->op_code != 0x01) {
            ESP_LOGD(TAG, "RX non-text frame opcode=0x%02X diabaikan", data->op_code);
            reset_assembly();
            return false;
        }

        reset_assembly();
        s_expected = payload_len;
        s_assembling = true;
    } else if (!s_assembling || data->op_code != 0x00 ||
               s_expected != payload_len || offset != s_received) {
        ESP_LOGW(TAG,
                 "RX fragment sequence error: opcode=0x%02X offset=%u received=%u expected=%u total=%u",
                 data->op_code,
                 (unsigned)offset,
                 (unsigned)s_received,
                 (unsigned)s_expected,
                 (unsigned)payload_len);
        reset_assembly();
        return false;
    }

    if (chunk_len > s_expected - s_received) {
        ESP_LOGW(TAG, "RX fragment overflow: chunk=%u remaining=%u",
                 (unsigned)chunk_len,
                 (unsigned)(s_expected - s_received));
        reset_assembly();
        return false;
    }

    memcpy(s_assembly + offset, data->data_ptr, chunk_len);
    s_received += chunk_len;

    if (s_received != s_expected) {
        return true;
    }

    s_assembly[s_expected] = '\0';

    char *message = static_cast<char *>(malloc(s_expected + 1));
    if (!message) {
        ESP_LOGW(TAG, "RX message malloc gagal: len=%u", (unsigned)s_expected);
        reset_assembly();
        return false;
    }

    memcpy(message, s_assembly, s_expected + 1);

    rx_message_t cmd = {
        .buffer = message,
        .len = s_expected,
    };

    reset_assembly();

    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Gemini RX queue penuh - message dibuang len=%u",
                 (unsigned)cmd.len);
        free(message);
        return false;
    }

    return true;
}
