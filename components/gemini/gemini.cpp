#include "gemini.h"
#include "gemini_protocol.h"
#include "websocket.h"
#include "esp_log.h"

namespace {
static const char *TAG = "GEMINI";
static bool s_initialized = false;
static bool s_started = false;

void on_connected(void *) { ESP_LOGI(TAG, "Gemini transport connected"); }
void on_disconnected(void *) { ESP_LOGW(TAG, "Gemini transport disconnected"); }
void on_text(const char *data, size_t length, void *) { gemini_protocol_on_text(data, length); }
void on_binary(const uint8_t *data, size_t length, void *) { gemini_protocol_on_binary(data, length); }
void on_error(void *) { ESP_LOGE(TAG, "Gemini transport error"); }
}

extern "C" bool gemini_init(const char *uri)
{
    if (!uri || uri[0] == '\0') return false;
    websocket_callbacks_t callbacks = {};
    callbacks.on_connected = on_connected;
    callbacks.on_disconnected = on_disconnected;
    callbacks.on_text = on_text;
    callbacks.on_binary = on_binary;
    callbacks.on_error = on_error;
    s_initialized = websocket_init(uri, &callbacks);
    return s_initialized;
}

extern "C" bool gemini_start(void)
{
    if (!s_initialized) return false;
    s_started = websocket_start();
    return s_started;
}

extern "C" void gemini_stop(void)
{
    websocket_stop();
    s_started = false;
}

extern "C" bool gemini_is_connected(void)
{
    return s_started && websocket_is_connected();
}

extern "C" bool gemini_send_audio(const uint8_t *data, size_t length)
{
    return s_started && gemini_protocol_send_audio(data, length);
}

extern "C" bool gemini_send_text(const char *data, size_t length)
{
    return s_started && gemini_protocol_send_text(data, length);
}
