#include "gemini_protocol.h"
#include "gemini_message.h"
#include "web_config.h"
#include "cJSON.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_PROTO";

static constexpr size_t ROLE_MAX = 2048;
static char s_session_handle[512] = {0};

bool gemini_protocol_build_setup(char **output, size_t *output_len)
{
    if (!output || !output_len) return false;

    *output = nullptr;
    *output_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON *generation = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(generation, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));

    cJSON *speech = cJSON_AddObjectToObject(generation, "speechConfig");
    cJSON_AddStringToObject(speech, "languageCode", "id-ID");
    cJSON *voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", "Kore");

    cJSON_AddStringToObject(setup, "model", "models/gemini-3.1-flash-live-preview");
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");

    cJSON *instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(instruction, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text",
        "Kamu adalah asisten suara berbahasa Indonesia. "
        "Jawab secara natural, singkat, dan jelas.");
    cJSON_AddItemToArray(parts, part);

    char role_text[ROLE_MAX] = {0};
    if (web_config_load_role(role_text, sizeof(role_text)) && role_text[0] != '\0') {
        cJSON *role_part = cJSON_CreateObject();
        if (role_part) {
            cJSON_AddStringToObject(role_part, "text", role_text);
            cJSON_AddItemToArray(parts, role_part);
        }
    }

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(realtime, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", false);
    cJSON_AddStringToObject(aad, "startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
    cJSON_AddNumberToObject(aad, "prefixPaddingMs", 40);
    cJSON_AddStringToObject(aad, "endOfSpeechSensitivity", "END_SENSITIVITY_HIGH");
    cJSON_AddNumberToObject(aad, "silenceDurationMs", 500);

    cJSON *resumption = cJSON_AddObjectToObject(setup, "sessionResumption");
    if (s_session_handle[0] != '\0')
        cJSON_AddStringToObject(resumption, "handle", s_session_handle);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        ESP_LOGE(TAG, "Gagal membuat Gemini setup JSON");
        return false;
    }

    *output = json;
    *output_len = strlen(json);
    ESP_LOGI(TAG, "Gemini setup siap: AUDIO, id-ID, Kore, AAD");
    return true;
}

bool gemini_protocol_process_message(const char *json, size_t len)
{
    if (!json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "Pesan Gemini bukan JSON valid (%u byte)", (unsigned)len);
        return false;
    }

    const gemini_message_type_t type = gemini_message_classify(json, len);
    bool handled = type != GEMINI_MESSAGE_UNKNOWN;

    switch (type) {
        case GEMINI_MESSAGE_SETUP:
            ESP_LOGI(TAG, "Gemini: setupComplete");
            break;
        case GEMINI_MESSAGE_AUDIO:
            ESP_LOGD(TAG, "Gemini: audio/serverContent");
            break;
        case GEMINI_MESSAGE_TEXT:
            ESP_LOGD(TAG, "Gemini: transcription/text");
            break;
        case GEMINI_MESSAGE_TOOL:
            ESP_LOGD(TAG, "Gemini: tool call");
            break;
        default:
            ESP_LOGD(TAG, "Gemini: pesan belum dipetakan");
            break;
    }

    cJSON_Delete(root);
    return handled;
}
