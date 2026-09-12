#include "gemini_protocol.h"
#include "websocket.h"
#include "esp_log.h"

namespace {
static const char *TAG = "GEMINI_PROTO";
}

bool gemini_protocol_send_audio(const uint8_t *data, size_t length)
{
    if (!data || !length) return false;
    return websocket_send_binary(data, length);
}

bool gemini_protocol_send_text(const char *data, size_t length)
{
    if (!data || !length) return false;
    return websocket_send_text(data, length);
}

void gemini_protocol_on_text(const char *data, size_t length)
{
    if (!data || !length) return;
    ESP_LOGD(TAG, "received text frame: %u bytes", (unsigned)length);
    // Gemini JSON parsing belongs here. No audio hardware access is allowed here.
}

void gemini_protocol_on_binary(const uint8_t *data, size_t length)
{
    if (!data || !length) return;
    ESP_LOGD(TAG, "received binary frame: %u bytes", (unsigned)length);
    // Binary payload decoding belongs here before handing PCM to audio_engine.
}
