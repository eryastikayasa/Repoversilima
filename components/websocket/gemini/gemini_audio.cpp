#include "gemini_audio.h"

#include "audio_engine.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_AUDIO";

static constexpr size_t AUDIO_RING_BUFFER_SIZE = 512 * 1024;
static constexpr size_t AUDIO_PLAYBACK_PREBUFFER_SIZE = 128 * 1024;
static constexpr size_t PLAYBACK_READ_SIZE = 2048;
static constexpr size_t PLAYBACK_TRIGGER_SIZE = 1024;
static constexpr uint32_t PLAYBACK_TASK_STACK = 4096;
static constexpr UBaseType_t PLAYBACK_TASK_PRIORITY = 6;
static constexpr uint32_t RING_SEND_TIMEOUT_MS = 20;

static StreamBufferHandle_t s_audio_ring = nullptr;
static StaticStreamBuffer_t s_audio_ring_struct;
static uint8_t *s_audio_ring_memory = nullptr;
static TaskHandle_t s_playback_task = nullptr;
static SemaphoreHandle_t s_audio_mutex = nullptr;
static StaticSemaphore_t s_audio_mutex_storage;

static volatile bool s_playback_active = false;
static volatile bool s_speaker_started = false;
static volatile bool s_turn_complete_pending = false;
static bool s_logged_first_audio = false;

static void playback_task(void *arg);

static bool ensure_playback_pipeline(void)
{
    if (s_audio_ring && s_playback_task && s_audio_mutex) return true;

    if (!s_audio_mutex) {
        s_audio_mutex = xSemaphoreCreateMutexStatic(&s_audio_mutex_storage);
        if (!s_audio_mutex) {
            ESP_LOGE(TAG, "Gagal membuat audio mutex");
            return false;
        }
    }

    if (!s_audio_ring) {
        s_audio_ring_memory = static_cast<uint8_t *>(
            heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_audio_ring_memory) {
            s_audio_ring_memory = static_cast<uint8_t *>(
                heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!s_audio_ring_memory) {
            ESP_LOGE(TAG, "Gagal alokasi audio ring %u byte", (unsigned)AUDIO_RING_BUFFER_SIZE);
            return false;
        }

        s_audio_ring = xStreamBufferCreateStatic(
            AUDIO_RING_BUFFER_SIZE,
            PLAYBACK_TRIGGER_SIZE,
            s_audio_ring_memory,
            &s_audio_ring_struct);
        if (!s_audio_ring) {
            ESP_LOGE(TAG, "Gagal membuat audio stream buffer");
            heap_caps_free(s_audio_ring_memory);
            s_audio_ring_memory = nullptr;
            return false;
        }
    }

    if (!s_playback_task) {
        if (xTaskCreate(playback_task, "gemini_playback", PLAYBACK_TASK_STACK,
                        nullptr, PLAYBACK_TASK_PRIORITY, &s_playback_task) != pdPASS) {
            ESP_LOGE(TAG, "Gagal membuat Gemini playback worker");
            return false;
        }
    }

    return true;
}

static void clear_audio_ring(void)
{
    if (s_audio_ring && s_audio_mutex) {
        if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            xStreamBufferReset(s_audio_ring);
            xSemaphoreGive(s_audio_mutex);
        }
    }

    s_turn_complete_pending = false;
    s_playback_active = false;

    if (s_speaker_started) {
        audio_engine_stop_playback();
        s_speaker_started = false;
    }
}

static bool queue_pcm_bytes(const uint8_t *data, size_t bytes)
{
    if (!data || bytes == 0 || (bytes & 1U) != 0) return false;
    if (!ensure_playback_pipeline()) return false;

    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Audio ring mutex timeout");
        return false;
    }

    size_t offset = 0;
    while (offset < bytes) {
        const size_t chunk = (bytes - offset > 8192) ? 8192 : (bytes - offset);
        const size_t written = xStreamBufferSend(
            s_audio_ring, data + offset, chunk, pdMS_TO_TICKS(RING_SEND_TIMEOUT_MS));
        offset += written;
        if (written < chunk) {
            ESP_LOGW(TAG, "Audio ring penuh: queued=%u/%u byte",
                     (unsigned)offset, (unsigned)bytes);
            break;
        }
    }

    xSemaphoreGive(s_audio_mutex);
    return offset == bytes;
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
        pcm, capacity, &decoded_len,
        reinterpret_cast<const unsigned char *>(encoded->valuestring), b64_len);

    if (rc != 0 || decoded_len == 0 || (decoded_len & 1U) != 0) {
        ESP_LOGW(TAG, "Decode PCM Base64 gagal: rc=%d bytes=%u", rc, (unsigned)decoded_len);
        free(pcm);
        return false;
    }

    // A tiny Gemini fragment is only queued. I2S is deliberately started
    // later by the playback worker after the Repo3-style 128 KB prebuffer.
    if (!s_playback_active) {
        s_playback_active = true;
        s_turn_complete_pending = false;
        s_logged_first_audio = false;
    }

    if (!s_logged_first_audio) {
        ESP_LOGI(TAG,
                 "Audio Gemini diterima: %u byte PCM16 -> ring %u byte; prebuffer=%u byte",
                 (unsigned)decoded_len,
                 (unsigned)AUDIO_RING_BUFFER_SIZE,
                 (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE);
        s_logged_first_audio = true;
    }

    const bool ok = queue_pcm_bytes(pcm, decoded_len);
    free(pcm);
    return ok;
}

static void playback_task(void *)
{
    static int16_t playback_buffer[PLAYBACK_READ_SIZE / sizeof(int16_t)];

    ESP_LOGI(TAG, "Gemini playback worker START: ring=%uKB prebuffer=%uKB",
             (unsigned)(AUDIO_RING_BUFFER_SIZE / 1024),
             (unsigned)(AUDIO_PLAYBACK_PREBUFFER_SIZE / 1024));

    for (;;) {
        size_t received = 0;
        size_t pending = 0;

        if (s_audio_ring && s_audio_mutex &&
            xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            pending = xStreamBufferBytesAvailable(s_audio_ring);

            const bool can_start = !s_speaker_started && s_playback_active &&
                                   (pending >= AUDIO_PLAYBACK_PREBUFFER_SIZE ||
                                    (s_turn_complete_pending && pending > 0));

            if (can_start) {
                if (audio_engine_start_playback()) {
                    s_speaker_started = true;
                    ESP_LOGI(TAG,
                             "Speaker PREBUFFER READY: %u/%u byte; AudioEngine playback START",
                             (unsigned)pending,
                             (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE);
                } else {
                    ESP_LOGE(TAG, "AudioEngine playback START gagal setelah prebuffer");
                }
            }

            if (s_speaker_started) {
                received = xStreamBufferReceive(
                    s_audio_ring,
                    reinterpret_cast<uint8_t *>(playback_buffer),
                    sizeof(playback_buffer),
                    pdMS_TO_TICKS(5));
            }

            pending = xStreamBufferBytesAvailable(s_audio_ring);
            xSemaphoreGive(s_audio_mutex);

            if (received > 0) {
                received &= ~((size_t)1);
                if (received > 0 &&
                    !audio_engine_write_speaker_pcm(
                        playback_buffer, received / sizeof(int16_t), 100)) {
                    ESP_LOGW(TAG, "AudioEngine speaker queue menolak %u byte", (unsigned)received);
                }
            }

            if (s_turn_complete_pending && pending == 0 && received == 0) {
                s_turn_complete_pending = false;
                s_playback_active = false;
                s_logged_first_audio = false;
                if (s_speaker_started) {
                    ESP_LOGI(TAG,
                             "AUDIO PLAYBACK COMPLETE: Gemini ring drain selesai; AudioEngine tail masih terjadwal");
                } else {
                    ESP_LOGI(TAG,
                             "AUDIO PLAYBACK COMPLETE: tidak ada PCM; SESSION tetap hidup; next turn READY");
                }
            }
        }

        if (received == 0) vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool gemini_audio_turn_active(void)
{
    return s_playback_active || s_turn_complete_pending;
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
        clear_audio_ring();
        s_logged_first_audio = false;
        ESP_LOGI(TAG, "Gemini interrupted: playback dihentikan dan ring dibersihkan");
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
            if (cJSON_IsObject(inline_data) && process_inline_audio(inline_data)) handled = true;
        }
    }

    cJSON *turn_complete = cJSON_GetObjectItemCaseSensitive(server_content, "turnComplete");
    if (cJSON_IsTrue(turn_complete)) {
        s_turn_complete_pending = true;
        ESP_LOGI(TAG, "TURN COMPLETE: Gemini selesai; playback drain; SESSION tetap HIDUP");
        handled = true;
    }

    cJSON_Delete(root);
    return handled;
}
