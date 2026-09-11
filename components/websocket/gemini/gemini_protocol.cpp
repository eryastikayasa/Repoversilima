#include "gemini_protocol.h"
#include "gemini_message.h"
#include "web_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_PROTO";
static constexpr size_t ROLE_MAX = 2048;
static constexpr size_t SESSION_HANDLE_MAX = 512;
static constexpr char AUDIO_MIME[] = "audio/pcm;rate=16000";
static char s_session_handle[SESSION_HANDLE_MAX] = {0};

static size_t base64_encoded_size(size_t input_len){return ((input_len+2U)/3U)*4U;}
static bool base64_encode(const uint8_t *input,size_t input_len,char *output,size_t output_size){
    static constexpr char TABLE[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if(!input||!output)return false;const size_t encoded_len=base64_encoded_size(input_len);if(output_size<encoded_len+1U)return false;size_t in=0,out=0;
    while(in+2U<input_len){const uint32_t value=(static_cast<uint32_t>(input[in])<<16)|(static_cast<uint32_t>(input[in+1U])<<8)|static_cast<uint32_t>(input[in+2U]);output[out++]=TABLE[(value>>18)&0x3F];output[out++]=TABLE[(value>>12)&0x3F];output[out++]=TABLE[(value>>6)&0x3F];output[out++]=TABLE[value&0x3F];in+=3U;}
    const size_t remaining=input_len-in;if(remaining==1U){const uint32_t value=static_cast<uint32_t>(input[in])<<16;output[out++]=TABLE[(value>>18)&0x3F];output[out++]=TABLE[(value>>12)&0x3F];output[out++]='=';output[out++]='=';}else if(remaining==2U){const uint32_t value=(static_cast<uint32_t>(input[in])<<16)|(static_cast<uint32_t>(input[in+1U])<<8);output[out++]=TABLE[(value>>18)&0x3F];output[out++]=TABLE[(value>>12)&0x3F];output[out++]=TABLE[(value>>6)&0x3F];output[out++]='=';}output[out]='\0';return true;
}

static void add_device_control_tool(cJSON *setup){
    cJSON *tools=cJSON_AddArrayToObject(setup,"tools");if(!tools)return;cJSON *tool=cJSON_CreateObject();cJSON *functions=cJSON_AddArrayToObject(tool,"functionDeclarations");cJSON *decl=cJSON_CreateObject();
    cJSON_AddStringToObject(decl,"name","control_device");cJSON_AddStringToObject(decl,"description","Mengontrol perangkat melalui UART. Gunakan hanya saat pengguna meminta aksi perangkat. Setiap command adalah satu kali tekan tombol fisik.");
    cJSON *parameters=cJSON_AddObjectToObject(decl,"parameters");cJSON_AddStringToObject(parameters,"type","OBJECT");cJSON *properties=cJSON_AddObjectToObject(parameters,"properties");cJSON *command=cJSON_AddObjectToObject(properties,"command");cJSON_AddStringToObject(command,"type","STRING");cJSON_AddStringToObject(command,"description","Command perangkat yang harus dijalankan tepat satu kali.");
    cJSON *values=cJSON_AddArrayToObject(command,"enum");static const char *const commands[]={"r1","r2","r3","r4","fan_pwr","fan_speed","fan_swing","fan_mode","mp3_mode","mp3_play","mp3_eq","m_led","m_mute","m_musik","m_cek","cek_suhu","cek_cahaya"};for(size_t i=0;i<sizeof(commands)/sizeof(commands[0]);++i)cJSON_AddItemToArray(values,cJSON_CreateString(commands[i]));
    cJSON *required=cJSON_AddArrayToObject(parameters,"required");cJSON_AddItemToArray(required,cJSON_CreateString("command"));cJSON_AddItemToArray(functions,decl);cJSON_AddItemToArray(tools,tool);
}

bool gemini_protocol_build_setup(char **output,size_t *output_len){
    if(!output||!output_len)return false;*output=nullptr;*output_len=0;cJSON *root=cJSON_CreateObject();if(!root)return false;cJSON *setup=cJSON_AddObjectToObject(root,"setup");
    cJSON *generation=cJSON_AddObjectToObject(setup,"generationConfig");cJSON *modalities=cJSON_AddArrayToObject(generation,"responseModalities");cJSON_AddItemToArray(modalities,cJSON_CreateString("AUDIO"));cJSON *speech=cJSON_AddObjectToObject(generation,"speechConfig");cJSON_AddStringToObject(speech,"languageCode","id-ID");cJSON *voice=cJSON_AddObjectToObject(speech,"voiceConfig");cJSON *prebuilt=cJSON_AddObjectToObject(voice,"prebuiltVoiceConfig");cJSON_AddStringToObject(prebuilt,"voiceName","Kore");
    cJSON_AddStringToObject(setup,"model","models/gemini-3.1-flash-live-preview");cJSON_AddObjectToObject(setup,"inputAudioTranscription");cJSON *instruction=cJSON_AddObjectToObject(setup,"systemInstruction");cJSON *parts=cJSON_AddArrayToObject(instruction,"parts");cJSON *part=cJSON_CreateObject();cJSON_AddStringToObject(part,"text","Kamu adalah asisten suara berbahasa Indonesia. Jawab secara natural, singkat, dan jelas. Jika pengguna meminta aksi perangkat, gunakan fungsi control_device dengan command yang tepat. Jangan mengarang command atau mengklaim mengetahui status fisik perangkat.");cJSON_AddItemToArray(parts,part);
    char role_text[ROLE_MAX]={0};if(web_config_load_role(role_text,sizeof(role_text))&&role_text[0]!='\0'){cJSON *role_part=cJSON_CreateObject();if(role_part){cJSON_AddStringToObject(role_part,"text",role_text);cJSON_AddItemToArray(parts,role_part);}}add_device_control_tool(setup);
    cJSON *realtime=cJSON_AddObjectToObject(setup,"realtimeInputConfig");cJSON *aad=cJSON_AddObjectToObject(realtime,"automaticActivityDetection");cJSON_AddBoolToObject(aad,"disabled",false);cJSON_AddStringToObject(aad,"startOfSpeechSensitivity","START_SENSITIVITY_HIGH");cJSON_AddNumberToObject(aad,"prefixPaddingMs",40);cJSON_AddStringToObject(aad,"endOfSpeechSensitivity","END_SENSITIVITY_HIGH");cJSON_AddNumberToObject(aad,"silenceDurationMs",500);cJSON *resumption=cJSON_AddObjectToObject(setup,"sessionResumption");if(s_session_handle[0]!='\0')cJSON_AddStringToObject(resumption,"handle",s_session_handle);
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return false;*output=json;*output_len=strlen(json);ESP_LOGI(TAG,"Gemini setup siap: AUDIO, id-ID, Kore, AAD, UART TOOL");return true;
}

bool gemini_protocol_build_realtime_audio(const int16_t *pcm16,size_t samples,char **output,size_t *output_len){
    if(!pcm16||samples==0||!output||!output_len)return false;*output=nullptr;*output_len=0;const size_t pcm_bytes=samples*sizeof(int16_t);const size_t encoded_len=base64_encoded_size(pcm_bytes);char *encoded=static_cast<char *>(malloc(encoded_len+1U));if(!encoded)return false;if(!base64_encode(reinterpret_cast<const uint8_t *>(pcm16),pcm_bytes,encoded,encoded_len+1U)){free(encoded);return false;}
    cJSON *root=cJSON_CreateObject();cJSON *realtime=root?cJSON_AddObjectToObject(root,"realtimeInput"):nullptr;cJSON *audio=realtime?cJSON_AddObjectToObject(realtime,"audio"):nullptr;if(!root||!realtime||!audio){cJSON_Delete(root);free(encoded);return false;}cJSON_AddStringToObject(audio,"data",encoded);cJSON_AddStringToObject(audio,"mimeType",AUDIO_MIME);char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);free(encoded);if(!json)return false;*output=json;*output_len=strlen(json);return true;
}

bool gemini_protocol_build_realtime_text(const char *text,char **output,size_t *output_len){
    if(!text||text[0]=='\0'||!output||!output_len)return false;*output=nullptr;*output_len=0;cJSON *root=cJSON_CreateObject();cJSON *realtime=root?cJSON_AddObjectToObject(root,"realtimeInput"):nullptr;if(!root||!realtime){cJSON_Delete(root);return false;}cJSON_AddStringToObject(realtime,"text",text);char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return false;*output=json;*output_len=strlen(json);return true;
}

bool gemini_protocol_process_message(const char *json,size_t len){
    if(!json||len==0)return false;cJSON *root=cJSON_ParseWithLength(json,len);if(!root){ESP_LOGW(TAG,"Pesan Gemini bukan JSON valid (%u byte)",(unsigned)len);return false;}const gemini_message_type_t type=gemini_message_classify(json,len);bool handled=type!=GEMINI_MESSAGE_UNKNOWN;
    switch(type){case GEMINI_MESSAGE_SETUP:ESP_LOGI(TAG,"Gemini: setupComplete");break;case GEMINI_MESSAGE_AUDIO:ESP_LOGD(TAG,"Gemini: audio/serverContent");break;case GEMINI_MESSAGE_TEXT:ESP_LOGD(TAG,"Gemini: transcription/text");break;case GEMINI_MESSAGE_TOOL:ESP_LOGD(TAG,"Gemini: tool call");break;case GEMINI_MESSAGE_SESSION_RESUMPTION:{cJSON *update=cJSON_GetObjectItem(root,"sessionResumptionUpdate");cJSON *resumable=cJSON_GetObjectItem(update,"resumable");cJSON *handle=cJSON_GetObjectItem(update,"newHandle");if(cJSON_IsTrue(resumable)&&cJSON_IsString(handle)&&handle->valuestring&&handle->valuestring[0]!='\0'){const size_t handle_len=strlen(handle->valuestring);if(handle_len<sizeof(s_session_handle)){memcpy(s_session_handle,handle->valuestring,handle_len+1U);ESP_LOGI(TAG,"Gemini session resumption handle diperbarui");}else ESP_LOGW(TAG,"Gemini session handle terlalu panjang");}else ESP_LOGD(TAG,"Gemini session belum resumable");break;}default:break;}cJSON_Delete(root);return handled;
}
