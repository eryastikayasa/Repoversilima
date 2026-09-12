#include "audio_engine.h"
#include "audio_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include <string.h>
namespace {
static const char *TAG="AUDIO_ENGINE";
static constexpr size_t FRAME=320, BLOCK=4096;
struct Block { uint8_t *p; size_t n; uint32_t gen; };
static QueueHandle_t q=nullptr; static volatile bool ready=false,capture=false,session=false;
static volatile audio_engine_state_t state=AUDIO_ENGINE_IDLE;
static audio_engine_mic_frame_cb_t listener=nullptr,sink=nullptr; static void *listener_ctx=nullptr,*sink_ctx=nullptr;
static int16_t mic[FRAME];
static void mic_task(void *) { for(;;) { if(!capture){vTaskDelay(pdMS_TO_TICKS(20));continue;} size_t n=0; if(audio_hal_read_pcm(mic,FRAME,&n)!=ESP_OK||n!=FRAME){state=AUDIO_ENGINE_ERROR;continue;} state=AUDIO_ENGINE_LISTENING; const uint8_t *p=(const uint8_t*)mic; if(listener)listener(p,sizeof(mic),listener_ctx); if(sink)sink(p,sizeof(mic),sink_ctx); } }
static void play_task(void *) { Block b{}; for(;;){ if(xQueueReceive(q,&b,portMAX_DELAY)!=pdTRUE)continue; if(!b.p)continue; if(audio_hal_start_playback()!=ESP_OK){heap_caps_free(b.p);state=AUDIO_ENGINE_ERROR;continue;} state=AUDIO_ENGINE_PLAYING; size_t off=0; while(off+1<b.n){size_t samples=(b.n-off)/2;if(samples>1024)samples=1024;size_t wr=0;if(audio_hal_write_pcm((const int16_t*)(b.p+off),samples,&wr)!=ESP_OK||!wr)break;off+=wr*2;}heap_caps_free(b.p);} }
}
extern "C" bool audio_engine_init(void){if(ready)return true;audio_hal_init();q=xQueueCreate(8,sizeof(Block));if(!q)return false;if(xTaskCreate(mic_task,"audio_mic",4096,nullptr,5,nullptr)!=pdPASS)return false;if(xTaskCreate(play_task,"audio_play",4096,nullptr,5,nullptr)!=pdPASS)return false;ready=true;ESP_LOGI(TAG,"READY: MIC->AudioEngine and AudioEngine->Speaker");return true;}
extern "C" audio_engine_state_t audio_engine_get_state(void){return state;}
extern "C" const char *audio_engine_state_name(audio_engine_state_t s){switch(s){case AUDIO_ENGINE_LISTENING:return "LISTENING";case AUDIO_ENGINE_PLAYING:return "PLAYING";case AUDIO_ENGINE_ERROR:return "ERROR";default:return "IDLE";}}
extern "C" bool audio_engine_set_mic_listener(audio_engine_mic_frame_cb_t cb,void *ctx){listener=cb;listener_ctx=ctx;return true;}
extern "C" bool audio_engine_set_mic_sink(audio_engine_mic_frame_cb_t cb,void *ctx){sink=cb;sink_ctx=ctx;return true;}
extern "C" bool audio_engine_start_capture(void){if(!ready)return false;if(capture)return true;if(audio_hal_start_capture()!=ESP_OK)return false;capture=true;state=AUDIO_ENGINE_LISTENING;return true;}
extern "C" void audio_engine_stop_capture(void){if(!capture)return;capture=false;audio_hal_stop_capture();if(!session)state=AUDIO_ENGINE_IDLE;}
extern "C" bool audio_engine_capture_active(void){return capture;}
extern "C" bool audio_engine_push_model_audio(const uint8_t *pcm,size_t len,uint32_t gen){if(!ready||!q||!pcm||!len||len>BLOCK)return false;len&=~(size_t)1;if(!len)return false;uint8_t *p=(uint8_t*)heap_caps_malloc(len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!p)p=(uint8_t*)heap_caps_malloc(len,MALLOC_CAP_8BIT);if(!p)return false;memcpy(p,pcm,len);Block b{p,len,gen};if(xQueueSend(q,&b,0)!=pdTRUE){heap_caps_free(p);return false;}return true;}
extern "C" bool audio_engine_push_model_audio_base64(const char *b64,size_t len,uint32_t gen){if(!b64||!len)return false;size_t out=0;if(mbedtls_base64_decode(nullptr,0,&out,(const unsigned char*)b64,len)!=MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)return false;uint8_t *p=(uint8_t*)heap_caps_malloc(out,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!p)p=(uint8_t*)heap_caps_malloc(out,MALLOC_CAP_8BIT);if(!p)return false;if(mbedtls_base64_decode(p,out,&out,(const unsigned char*)b64,len)!=0){heap_caps_free(p);return false;}bool ok=audio_engine_push_model_audio(p,out,gen);heap_caps_free(p);return ok;}
extern "C" void audio_engine_start_input_session(void){session=true;audio_engine_start_capture();}
extern "C" void audio_engine_stop_input_session(void){session=false;audio_engine_stop_capture();}
extern "C" bool audio_engine_input_session_active(void){return session;}
