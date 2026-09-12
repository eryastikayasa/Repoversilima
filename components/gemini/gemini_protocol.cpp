#include "gemini_protocol.h"
#include "websocket.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace { static const char *TAG="GEMINI_PROTO"; }

bool gemini_protocol_init(void){return true;}
void gemini_protocol_reset(void){}
void gemini_protocol_on_connected(void){ESP_LOGI(TAG,"Gemini session connected");}

bool gemini_protocol_send_audio(const uint8_t *data,size_t length){
 if(!data||!length)return false; size_t enc=0;
 if(mbedtls_base64_encode(nullptr,0,&enc,data,length)!=MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)return false;
 char *b64=(char*)malloc(enc+1); if(!b64)return false;
 if(mbedtls_base64_encode((unsigned char*)b64,enc,&enc,data,length)!=0){free(b64);return false;} b64[enc]=0;
 size_t cap=enc+128; char *json=(char*)malloc(cap); if(!json){free(b64);return false;}
 int n=snprintf(json,cap,"{\"realtimeInput\":{\"mediaChunks\":[{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}]}}",b64);
 free(b64); bool ok=n>0&&(size_t)n<cap&&websocket_send_text(json,(size_t)n); free(json); return ok;
}

bool gemini_protocol_send_text(const char *data,size_t length){return data&&length&&websocket_send_text(data,length);}

void gemini_protocol_on_text(const char *data,size_t length){
 if(!data||!length)return; cJSON *root=cJSON_ParseWithLength(data,length); if(!root)return;
 cJSON *server=cJSON_GetObjectItemCaseSensitive(root,"serverContent");
 if(server){
  cJSON *turn=cJSON_GetObjectItemCaseSensitive(server,"modelTurn");
  cJSON *parts=turn?cJSON_GetObjectItemCaseSensitive(turn,"parts"):nullptr;
  if(cJSON_IsArray(parts)){cJSON *part=nullptr;cJSON_ArrayForEach(part,parts){
   cJSON *in=cJSON_GetObjectItemCaseSensitive(part,"inlineData"); cJSON *b=cJSON_GetObjectItemCaseSensitive(in,"data");
   if(cJSON_IsString(b)&&b->valuestring)audio_engine_push_model_audio_base64(b->valuestring,strlen(b->valuestring),0);
  }}
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server,"turnComplete")))ESP_LOGI(TAG,"Gemini turn complete");
 }
 cJSON_Delete(root);
}
void gemini_protocol_on_binary(const uint8_t *data,size_t length){if(data&&length)audio_engine_push_model_audio(data,length,0);}
