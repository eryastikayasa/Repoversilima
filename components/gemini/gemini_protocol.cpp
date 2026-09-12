#include "gemini_protocol.h"
#include "websocket.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
namespace {
static const char *TAG="GEMINI_PROTO"; static QueueHandle_t rxq=nullptr; static TaskHandle_t task=nullptr;
struct Rx {uint8_t *p;size_t n;bool text;};
static void process_text(const char *data,size_t length){cJSON *root=cJSON_ParseWithLength(data,length);if(!root)return;cJSON *server=cJSON_GetObjectItemCaseSensitive(root,"serverContent");if(server){cJSON *turn=cJSON_GetObjectItemCaseSensitive(server,"modelTurn");cJSON *parts=turn?cJSON_GetObjectItemCaseSensitive(turn,"parts"):nullptr;if(cJSON_IsArray(parts)){cJSON *part=nullptr;cJSON_ArrayForEach(part,parts){cJSON *in=cJSON_GetObjectItemCaseSensitive(part,"inlineData");cJSON *b=in?cJSON_GetObjectItemCaseSensitive(in,"data"):nullptr;if(cJSON_IsString(b)&&b->valuestring)audio_engine_push_model_audio_base64(b->valuestring,strlen(b->valuestring),0);}}if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server,"turnComplete")))ESP_LOGI(TAG,"Gemini turn complete");}cJSON_Delete(root);}
static void rx_task(void *){Rx r{};for(;;){if(xQueueReceive(rxq,&r,portMAX_DELAY)!=pdTRUE)continue;if(r.p){if(r.text)process_text((const char*)r.p,r.n);else audio_engine_push_model_audio(r.p,r.n,0);heap_caps_free(r.p);}}}
}
bool gemini_protocol_init(void){if(rxq)return true;rxq=xQueueCreate(6,sizeof(Rx));if(!rxq)return false;return xTaskCreate(rx_task,"gemini_rx",6144,nullptr,6,&task)==pdPASS;}
void gemini_protocol_reset(void){Rx r{};if(!rxq)return;while(xQueueReceive(rxq,&r,0)==pdTRUE)if(r.p)heap_caps_free(r.p);}
void gemini_protocol_on_connected(void){ESP_LOGI(TAG,"Gemini session connected");}
bool gemini_protocol_send_audio(const uint8_t *data,size_t length){if(!data||!length)return false;size_t enc=0;if(mbedtls_base64_encode(nullptr,0,&enc,data,length)!=MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)return false;char *b64=(char*)malloc(enc+1);if(!b64)return false;if(mbedtls_base64_encode((unsigned char*)b64,enc,&enc,data,length)!=0){free(b64);return false;}b64[enc]=0;size_t cap=enc+128;char *json=(char*)malloc(cap);if(!json){free(b64);return false;}int n=snprintf(json,cap,"{\"realtimeInput\":{\"mediaChunks\":[{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}]}}",b64);free(b64);bool ok=n>0&&(size_t)n<cap&&websocket_send_text(json,(size_t)n);free(json);return ok;}
bool gemini_protocol_send_text(const char *data,size_t length){return data&&length&&websocket_send_text(data,length);}
void gemini_protocol_on_text(const char *data,size_t length){if(!data||!length||!rxq)return;uint8_t *p=(uint8_t*)heap_caps_malloc(length+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!p)p=(uint8_t*)heap_caps_malloc(length+1,MALLOC_CAP_8BIT);if(!p)return;memcpy(p,data,length);p[length]=0;Rx r{p,length,true};if(xQueueSend(rxq,&r,0)!=pdTRUE)heap_caps_free(p);}
void gemini_protocol_on_binary(const uint8_t *data,size_t length){if(!data||!length||!rxq)return;uint8_t *p=(uint8_t*)heap_caps_malloc(length,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!p)p=(uint8_t*)heap_caps_malloc(length,MALLOC_CAP_8BIT);if(!p)return;memcpy(p,data,length);Rx r{p,length,false};if(xQueueSend(rxq,&r,0)!=pdTRUE)heap_caps_free(p);}
