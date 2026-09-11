#include "gemini_audio.h"

#include "audio_engine.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_AUDIO";
static bool s_playback_active = false;
static bool s_logged_first_audio = false;

static bool write_pcm_bytes(const uint8_t *data, size_t bytes)
{
    if (!data || bytes == 0 || (bytes & 1U) != 0) return false;

    const int16_t *pcm = reinterpret_cast<const int16_t *>(data);
    const size_t samples = bytes / sizeof(int16_t);

    // AudioEngine owns the speaker path. WebSocket/Gemini only hands PCM over.
    return audio_engine_write_speaker_pcm(pcm, samples, 20);
}

static bool process_inline_audio(cJSON *inline_data)
{
    cJSON *encoded = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
    if (!cJSON_IsString(encoded) || !encoded->valuestring || encoded->valuestring[0] == '\0') {
        return false;
    }

    cJSON *mime = cJSON_GetObjectItemCaseSensitive(inline_data, "mimeType");
    if (cJSON_IsString(mime) && mime->valuestring &&
        strncmp(mime->valuestring, "audio/pcm", strlen("audio/pcm")) != 0) {
        ESP_LOGW(TAG, "inlineData mimeType bukan PCM: %s", mime->valuestring);
        return false;
    }

    const size_t b64_len = strlen(encoded->valuestring);
    const size_t capacity = (b64_len / 4U) * 3U + 3U;
    uint8_t *pcm = static_cast<uint8_t *>(malloc(capacity));
    if (!pcm) {
        ESP_LOGE(TAG, "Gagal alokasi buffer decode Base64: %u byte", (unsigned)capacity);
        return false;
    }

    size_t decoded_len = 0;
    const int rc = mbedtls_base64_decode(
        pcm,
        capacity,
        &decoded_len,
        reinterpret_cast<const unsigned char *>(encoded->valuestring),
        b64_len);

    if (rc != 0 || decoded_len == 0 || (decoded_len & 1U) != 0) {
        ESP_LOGW(TAG, "Decode PCM Base64 gagal: rc=%d bytes=%u", rc, (unsigned)decoded_len);
        free(pcm);
        return false;
    }

    if (!s_playback_active) {
        if (!audio_engine_start_playback()) {
            ESP_LOGE(TAG, "AudioEngine playback START gagal");
            free(pcm);
            return false;
        }
        s_playback_active = true;
        s_logged_first_audio = false;
    }

    if (!s_logged_first_audio) {
        ESP_LOGI(TAG, "Audio Gemini diterima: %u byte PCM16", (unsigned)decoded_len);
        s_logged_first_audio = true;
    }

    const bool ok = write_pcm_bytes(pcm, decoded_len);
    free(pcm);
    return ok;
}

bool gemini_audio_process_server_message(const char *json, size_t len)
{
    if (!json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool handled = false;
    cJSON *server_content = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (!cJSON_IsObject(server_content)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *interrupted = cJSON_GetObjectItemCaseSensitive(server_content, "interrupted");
    if (cJSON_IsTrue(interrupted)) {
        if (s_playback_active) {
            audio_engine_stop_playback();
            s_playback_active = false;
        }
        s_logged_first_audio = false;
        ESP_LOGI(TAG, "Gemini interrupted: playback dihentikan");
        handled = true;
    }

    cJSON *model_turn = cJSON_GetObjectItemCaseSensitive(server_content, "modelTurn");
    cJSON *parts = model_turn ? cJSON_GetObjectItemCaseSensitive(model_turn, "parts") : nullptr;
    if (cJSON_IsArray(parts)) {
        const int count = cJSON_GetArraySize(parts);
        for (int i = 0; i < count; ++i) {
            cJSON *part = cJSON_GetArrayItem(parts, i);
            if (!cJSON_IsObject(part)) continue;

            cJSON *inline_data = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
            if (cJSON_IsObject(inline_data) && process_inline_audio(inline_data)) {
                handled = true;
            }
        }
    }

    // turnComplete only marks the end of Gemini's model turn. AudioEngine may
    // still have buffered PCM to play, so leave playback running to drain.
    cJSON *turn_complete = cJSON_GetObjectItemCaseSensitive(server_content, "turnComplete");
    if (cJSON_IsTrue(turn_complete)) {
        ESP_LOGI(TAG, "Turn complete: PCM playback dibiarkan drain");
        handled = true;
    }

    cJSON_Delete(root);
    return handled;
}
