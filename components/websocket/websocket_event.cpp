#include "websocket_event.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "gemini_message.h"
#include "gemini_audio.h"
#include "uart_control.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG="WS_EVENT";
static volatile bool s_gemini_ready=false;
static volatile int64_t s_last_activity_us=0;
static volatile bool s_rx_processing=false;
static bool s_greeting_sent=false;
static constexpr size_t RX_MAX_PAYLOAD=64*1024;
static constexpr size_t RX_BUFFER_COUNT=10;
static constexpr uint32_t RX_WORKER_STACK=8192;
static constexpr UBaseType_t RX_WORKER_PRIORITY=5;
static constexpr uint32_t SETUP_TASK_STACK=4096;
static constexpr UBaseType_t SETUP_TASK_PRIORITY=5;
static constexpr TickType_t RX_BACKPRESSURE_WAIT=pdMS_TO_TICKS(20);
static QueueHandle_t s_rx_free_queue=nullptr;
static QueueHandle_t s_rx_ready_queue=nullptr;
static StaticQueue_t s_rx_free_queue_storage;
static StaticQueue_t s_rx_ready_queue_storage;
static char *s_rx_free_storage[RX_BUFFER_COUNT];
static char *s_rx_ready_storage[RX_BUFFER_COUNT];
static char *s_rx_buffers[RX_BUFFER_COUNT]={};
static TaskHandle_t s_rx_worker_task=nullptr;
static bool s_rx_worker_ready=false;
static char *s_rx_assembling_buffer=nullptr;
static size_t s_rx_expected=0,s_rx_received=0;
static bool s_rx_assembling=false;

static bool send_tool_response(const char *id,const char *name,bool success)
{
    if(!id||!name||!websocket_transport_is_connected())return false;
    const char *sensor=success?uart_control_take_last_sensor_response():nullptr;
    const char *result=(sensor&&sensor[0])?sensor:(success?"ok":"error");
    cJSON *root=cJSON_CreateObject();cJSON *tr=root?cJSON_AddObjectToObject(root,"toolResponse"):nullptr;cJSON *responses=tr?cJSON_AddArrayToObject(tr,"functionResponses"):nullptr;cJSON *response=responses?cJSON_CreateObject():nullptr;
    if(!root||!tr||!responses||!response){cJSON_Delete(root);return false;}
    cJSON_AddStringToObject(response,"id",id);cJSON_AddStringToObject(response,"name",name);cJSON *body=cJSON_AddObjectToObject(response,"response");
    if(!body){cJSON_Delete(root);return false;}cJSON_AddStringToObject(body,"result",result);cJSON_AddItemToArray(responses,response);
    char *payload=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!payload)return false;const esp_err_t err=websocket_transport_send_text(payload,strlen(payload));free(payload);
    if(err!=ESP_OK){ESP_LOGW(TAG,"Tool response gagal dikirim: %s",esp_err_to_name(err));return false;}ESP_LOGI(TAG,"UART tool response: %s",result);return true;
}

static bool process_gemini_tool_call(const char *json,size_t len)
{
    cJSON *root=cJSON_ParseWithLength(json,len);if(!root)return false;cJSON *tool_call=cJSON_GetObjectItem(root,"toolCall");cJSON *calls=tool_call?cJSON_GetObjectItem(tool_call,"functionCalls"):nullptr;
    if(!cJSON_IsArray(calls)){cJSON_Delete(root);return false;}
    bool handled=false;cJSON *fc=nullptr;cJSON_ArrayForEach(fc,calls){if(!cJSON_IsObject(fc))continue;cJSON *id=cJSON_GetObjectItem(fc,"id");cJSON *name=cJSON_GetObjectItem(fc,"name");cJSON *args=cJSON_GetObjectItem(fc,"args");
        if(!cJSON_IsString(id)||!id->valuestring||!cJSON_IsString(name)||!name->valuestring||!cJSON_IsObject(args))continue;
        if(strcmp(name->valuestring,"control_device")!=0)continue;
        cJSON *command=cJSON_GetObjectItem(args,"command");
        if(!cJSON_IsString(command)||!command->valuestring)continue;
        ESP_LOGI(TAG,"Gemini TOOL: %s",command->valuestring);const bool success=uart_control_execute_command(command->valuestring);send_tool_response(id->valuestring,name->valuestring,success);handled=true;
    }
    cJSON_Delete(root);return handled;
}

static bool ensure_rx_worker(void)
{
    if(s_rx_worker_ready)return true;
    s_rx_free_queue=xQueueCreateStatic(RX_BUFFER_COUNT,sizeof(char *),reinterpret_cast<uint8_t *>(s_rx_free_storage),&s_rx_free_queue_storage);s_rx_ready_queue=xQueueCreateStatic(RX_BUFFER_COUNT,sizeof(char *),reinterpret_cast<uint8_t *>(s_rx_ready_storage),&s_rx_ready_queue_storage);if(!s_rx_free_queue||!s_rx_ready_queue)return false;
    for(size_t i=0;i<RX_BUFFER_COUNT;++i){s_rx_buffers[i]=static_cast<char *>(heap_caps_malloc(RX_MAX_PAYLOAD+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(!s_rx_buffers[i]){ESP_LOGE(TAG,"Gagal alokasi RX buffer PSRAM #%u",(unsigned)i);return false;}if(xQueueSend(s_rx_free_queue,&s_rx_buffers[i],0)!=pdPASS)return false;}
    BaseType_t result=xTaskCreate([](void *){while(true){char *json=nullptr;if(xQueueReceive(s_rx_ready_queue,&json,portMAX_DELAY)!=pdPASS)continue;if(!json)continue;s_rx_processing=true;const size_t len=strlen(json);const gemini_message_type_t type=gemini_message_classify(json,len);const bool protocol_handled=gemini_protocol_process_message(json,len);const bool tool_handled=(type==GEMINI_MESSAGE_TOOL)&&process_gemini_tool_call(json,len);const bool audio_handled=gemini_audio_process_server_message(json,len);const bool handled=protocol_handled||tool_handled||audio_handled;
        if(type==GEMINI_MESSAGE_SETUP){s_gemini_ready=true;ESP_LOGI(TAG,"Gemini setupComplete - audio uplink READY");if(!s_greeting_sent&&websocket_transport_is_connected()){char *greeting=nullptr;size_t greeting_len=0;if(gemini_protocol_build_realtime_text("halo ",&greeting,&greeting_len)){const esp_err_t err=websocket_transport_send_text(greeting,greeting_len);if(err==ESP_OK){s_greeting_sent=true;ESP_LOGI(TAG,"Gemini greeting trigger terkirim");}else ESP_LOGE(TAG,"Gagal mengirim Gemini greeting trigger: %s",esp_err_to_name(err));free(greeting);}else ESP_LOGE(TAG,"Gagal membuat Gemini greeting trigger");}}
        if(!handled){ESP_LOGW(TAG,"Gemini RX message tidak dipetakan (%u byte)");}while(xQueueSend(s_rx_free_queue,&json,RX_BACKPRESSURE_WAIT)!=pdPASS){}s_rx_processing=false;}},"ws_rx",RX_WORKER_STACK,nullptr,RX_WORKER_PRIORITY,&s_rx_worker_task);if(result!=pdPASS)return false;s_rx_worker_ready=true;return true;
}

bool websocket_event_init(void){return ensure_rx_worker();}
static void reset_rx(void){if(s_rx_assembling_buffer){if(s_rx_free_queue)while(xQueueSend(s_rx_free_queue,&s_rx_assembling_buffer,RX_BACKPRESSURE_WAIT)!=pdPASS){}s_rx_assembling_buffer=nullptr;}s_rx_expected=0;s_rx_received=0;s_rx_assembling=false;}
static bool acquire_rx_buffer(char **buffer){if(!buffer||!s_rx_free_queue)return false;while(xQueueReceive(s_rx_free_queue,buffer,RX_BACKPRESSURE_WAIT)!=pdPASS){if(!websocket_transport_is_connected())return false;}return *buffer!=nullptr;}
static void handle_data_event(esp_websocket_event_data_t *data){if(!data||!data->data_ptr||data->data_len<=0||data->payload_len<=0)return;const size_t payload_len=(size_t)data->payload_len,offset=(size_t)data->payload_offset,chunk_len=(size_t)data->data_len;if(payload_len>RX_MAX_PAYLOAD||offset>payload_len||chunk_len>payload_len-offset){ESP_LOGW(TAG,"RX payload tidak valid: total=%u max=%u",(unsigned)payload_len,(unsigned)RX_MAX_PAYLOAD);reset_rx();return;}if(!s_rx_worker_ready||!s_rx_free_queue||!s_rx_ready_queue){ESP_LOGE(TAG,"RX worker belum siap");reset_rx();return;}s_last_activity_us=esp_timer_get_time();if(offset==0){reset_rx();if(!acquire_rx_buffer(&s_rx_assembling_buffer)){ESP_LOGW(TAG,"RX tidak dapat memperoleh buffer karena WebSocket sudah putus");return;}s_rx_expected=payload_len;s_rx_received=0;s_rx_assembling=true;}else if(!s_rx_assembling||s_rx_expected!=payload_len||offset!=s_rx_received){ESP_LOGW(TAG,"RX fragment sequence tidak valid: offset=%u received=%u expected=%u total=%u",(unsigned)offset,(unsigned)s_rx_received,(unsigned)s_rx_expected,(unsigned)payload_len);reset_rx();return;}memcpy(s_rx_assembling_buffer+offset,data->data_ptr,chunk_len);s_rx_received+=chunk_len;if(s_rx_received!=s_rx_expected)return;s_rx_assembling_buffer[s_rx_expected]='\0';char *ready_buffer=s_rx_assembling_buffer;s_rx_assembling_buffer=nullptr;s_rx_expected=0;s_rx_received=0;s_rx_assembling=false;while(websocket_transport_is_connected()&&xQueueSend(s_rx_ready_queue,&ready_buffer,RX_BACKPRESSURE_WAIT)!=pdPASS){}if(!websocket_transport_is_connected()){ESP_LOGW(TAG,"RX payload selesai tetapi koneksi putus; payload di-abort bersama lifecycle");(void)xQueueSend(s_rx_free_queue,&ready_buffer,0);}}
static void send_gemini_setup(void){char *setup=nullptr;size_t setup_len=0;if(!gemini_protocol_build_setup(&setup,&setup_len)){ESP_LOGE(TAG,"Gagal membuat Gemini setup");return;}const esp_err_t err=websocket_transport_send_text(setup,setup_len);if(err==ESP_OK)ESP_LOGI(TAG,"Gemini setup terkirim (%u byte)",(unsigned)setup_len);else ESP_LOGE(TAG,"Gagal mengirim Gemini setup: %s",esp_err_to_name(err));free(setup);}
static void send_gemini_setup_task(void *arg){(void)arg;send_gemini_setup();vTaskDelete(nullptr);}
void websocket_event_handler(void *handler_args,esp_event_base_t base,int32_t event_id,void *event_data){(void)handler_args;(void)base;websocket_transport_handle_event(event_id,event_data);switch(event_id){case WEBSOCKET_EVENT_CONNECTED:s_gemini_ready=false;s_greeting_sent=false;s_last_activity_us=esp_timer_get_time();reset_rx();if(!s_rx_worker_ready)break;if(xTaskCreate(send_gemini_setup_task,"ws_setup",SETUP_TASK_STACK,nullptr,SETUP_TASK_PRIORITY,nullptr)!=pdPASS)ESP_LOGE(TAG,"Gagal membuat task Gemini setup");break;case WEBSOCKET_EVENT_DISCONNECTED:case WEBSOCKET_EVENT_ERROR:s_gemini_ready=false;s_greeting_sent=false;reset_rx();break;case WEBSOCKET_EVENT_DATA:handle_data_event(static_cast<esp_websocket_event_data_t *>(event_data));break;default:break;}}
bool websocket_event_gemini_ready(void){return s_gemini_ready;}void websocket_event_note_activity(void){s_last_activity_us=esp_timer_get_time();}int64_t websocket_event_last_activity_us(void){return s_last_activity_us;}
bool websocket_event_drain(void){if(!s_rx_worker_ready)return true;const int64_t deadline=esp_timer_get_time()+15000000LL;while(esp_timer_get_time()<deadline){if(uxQueueMessagesWaiting(s_rx_ready_queue)==0&&!s_rx_processing&&!s_rx_assembling)return true;vTaskDelay(pdMS_TO_TICKS(20));}ESP_LOGE(TAG,"RX drain timeout: ready=%u processing=%d assembling=%d",(unsigned)uxQueueMessagesWaiting(s_rx_ready_queue),(int)s_rx_processing,(int)s_rx_assembling);return false;}
