#include "audio_engine.h"
#include "audio_hal.h"
#include "wakeword.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

namespace {
static const char *TAG = "AUDIO_ENGINE";
static constexpr size_t PCM_BLOCK_SAMPLES = 512;
static constexpr uint32_t WAKEWORD_TASK_DELAY_MS = 1;
static constexpr uint32_t WAKEWORD_TASK_STACK = 8192;
static constexpr size_t MIC_FRAME_SAMPLES = 320;
static constexpr size_t MIC_FRAME_BYTES = MIC_FRAME_SAMPLES * sizeof(int16_t);
static constexpr size_t MIC_QUEUE_DEPTH = 250;
static constexpr uint32_t CONVERSATION_TASK_STACK = 4096;
static constexpr size_t PLAYBACK_BLOCK_SAMPLES = 1024;
static constexpr size_t PLAYBACK_BLOCK_BYTES = PLAYBACK_BLOCK_SAMPLES * sizeof(int16_t);
static constexpr size_t PLAYBACK_QUEUE_DEPTH = 94;
static constexpr uint32_t PLAYBACK_TASK_STACK = 4096;
static constexpr TickType_t BUFFER_WAIT_TICKS = pdMS_TO_TICKS(20);

struct PlaybackBlock { uint16_t samples; int16_t pcm[PLAYBACK_BLOCK_SAMPLES]; };
static int16_t s_pcm_buffer[PCM_BLOCK_SAMPLES];
static TaskHandle_t s_wakeword_task = nullptr;
static bool s_initialized = false;
static volatile bool s_wakeword_running = false;
static volatile bool s_wakeword_detected = false;
static TaskHandle_t s_conversation_task = nullptr;
static volatile bool s_conversation_running = false;
static StaticQueue_t s_mic_free_queue_storage, s_mic_ready_queue_storage;
static uint8_t *s_mic_free_queue_buffer[MIC_QUEUE_DEPTH], *s_mic_ready_queue_buffer[MIC_QUEUE_DEPTH];
static QueueHandle_t s_mic_free_queue = nullptr, s_mic_ready_queue = nullptr;
static uint8_t *s_mic_buffers[MIC_QUEUE_DEPTH] = {};
static TaskHandle_t s_playback_task = nullptr;
static volatile bool s_playback_running = false;
static volatile bool s_playback_processing = false;
static StaticQueue_t s_playback_free_queue_storage, s_playback_ready_queue_storage;
static PlaybackBlock *s_playback_free_queue_buffer[PLAYBACK_QUEUE_DEPTH], *s_playback_ready_queue_buffer[PLAYBACK_QUEUE_DEPTH];
static QueueHandle_t s_playback_free_queue = nullptr, s_playback_ready_queue = nullptr;
static PlaybackBlock *s_playback_buffers[PLAYBACK_QUEUE_DEPTH] = {};

static void free_audio_pools(void) {
    for (size_t i=0;i<MIC_QUEUE_DEPTH;++i) { if(s_mic_buffers[i]) { heap_caps_free(s_mic_buffers[i]); s_mic_buffers[i]=nullptr; } }
    for (size_t i=0;i<PLAYBACK_QUEUE_DEPTH;++i) { if(s_playback_buffers[i]) { heap_caps_free(s_playback_buffers[i]); s_playback_buffers[i]=nullptr; } }
}
static bool init_audio_pools(void) {
    s_mic_free_queue=xQueueCreateStatic(MIC_QUEUE_DEPTH,sizeof(uint8_t *),reinterpret_cast<uint8_t *>(s_mic_free_queue_buffer),&s_mic_free_queue_storage);
    s_mic_ready_queue=xQueueCreateStatic(MIC_QUEUE_DEPTH,sizeof(uint8_t *),reinterpret_cast<uint8_t *>(s_mic_ready_queue_buffer),&s_mic_ready_queue_storage);
    s_playback_free_queue=xQueueCreateStatic(PLAYBACK_QUEUE_DEPTH,sizeof(PlaybackBlock *),reinterpret_cast<uint8_t *>(s_playback_free_queue_buffer),&s_playback_free_queue_storage);
    s_playback_ready_queue=xQueueCreateStatic(PLAYBACK_QUEUE_DEPTH,sizeof(PlaybackBlock *),reinterpret_cast<uint8_t *>(s_playback_ready_queue_buffer),&s_playback_ready_queue_storage);
    if(!s_mic_free_queue||!s_mic_ready_queue||!s_playback_free_queue||!s_playback_ready_queue)return false;
    for(size_t i=0;i<MIC_QUEUE_DEPTH;++i){s_mic_buffers[i]=static_cast<uint8_t *>(heap_caps_malloc(MIC_FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(!s_mic_buffers[i]){free_audio_pools();return false;}if(xQueueSend(s_mic_free_queue,&s_mic_buffers[i],0)!=pdPASS){free_audio_pools();return false;}}
    for(size_t i=0;i<PLAYBACK_QUEUE_DEPTH;++i){s_playback_buffers[i]=static_cast<PlaybackBlock *>(heap_caps_malloc(PLAYBACK_BLOCK_BYTES+sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(!s_playback_buffers[i]){free_audio_pools();return false;}s_playback_buffers[i]->samples=0;if(xQueueSend(s_playback_free_queue,&s_playback_buffers[i],0)!=pdPASS){free_audio_pools();return false;}}
    ESP_LOGI(TAG,"Audio buffers siap (PSRAM)");return true;
}
static void wakeword_task(void *) { while(s_wakeword_running){if(audio_engine_process_wakeword())ESP_LOGI(TAG,"WakeWord terdeteksi");vTaskDelay(pdMS_TO_TICKS(WAKEWORD_TASK_DELAY_MS));}s_wakeword_task=nullptr;vTaskDelete(nullptr); }
static void conversation_task(void *) {
    static int16_t read_buffer[PCM_BLOCK_SAMPLES]; static uint8_t frame_buffer[MIC_FRAME_BYTES]; size_t frame_pos=0;
    while(s_conversation_running){size_t samples_read=0;esp_err_t err=audio_hal_read_pcm(read_buffer,PCM_BLOCK_SAMPLES,&samples_read);if(err!=ESP_OK){if(s_conversation_running)vTaskDelay(pdMS_TO_TICKS(1));continue;}if(!samples_read){vTaskDelay(pdMS_TO_TICKS(1));continue;}size_t offset=0;const uint8_t *raw=reinterpret_cast<const uint8_t *>(read_buffer);size_t bytes_read=samples_read*sizeof(int16_t);while(offset<bytes_read&&s_conversation_running){size_t remaining_frame=MIC_FRAME_BYTES-frame_pos,remaining_input=bytes_read-offset,copy_len=remaining_frame<remaining_input?remaining_frame:remaining_input;memcpy(frame_buffer+frame_pos,raw+offset,copy_len);frame_pos+=copy_len;offset+=copy_len;if(frame_pos!=MIC_FRAME_BYTES)continue;frame_pos=0;uint8_t *frame=nullptr;while(s_conversation_running&&xQueueReceive(s_mic_free_queue,&frame,BUFFER_WAIT_TICKS)!=pdTRUE){}if(!s_conversation_running)break;memcpy(frame,frame_buffer,MIC_FRAME_BYTES);while(s_conversation_running&&xQueueSend(s_mic_ready_queue,&frame,BUFFER_WAIT_TICKS)!=pdTRUE){}if(!s_conversation_running){(void)xQueueSend(s_mic_free_queue,&frame,0);break;}}}
    s_conversation_task=nullptr;vTaskDelete(nullptr);
}
static void playback_task(void *) {
    PlaybackBlock *block=nullptr;
    while(s_playback_running){if(xQueueReceive(s_playback_ready_queue,&block,pdMS_TO_TICKS(20))!=pdTRUE)continue;if(!block)continue;s_playback_processing=true;size_t offset=0;while(s_playback_running&&offset<block->samples){size_t written=0;esp_err_t err=audio_hal_write_pcm(block->pcm+offset,block->samples-offset,&written);if(written>(block->samples-offset))written=0;offset+=written;if(offset<block->samples&&(err!=ESP_OK||written==0))vTaskDelay(pdMS_TO_TICKS(5));}block->samples=0;(void)xQueueSend(s_playback_free_queue,&block,BUFFER_WAIT_TICKS);s_playback_processing=false;}
    s_playback_task=nullptr;vTaskDelete(nullptr);
}
static bool stop_wakeword_and_wait(void){if(!s_wakeword_running&&s_wakeword_task==nullptr)return true;s_wakeword_running=false;(void)audio_hal_stop_capture();for(uint32_t i=0;i<100&&s_wakeword_task;++i)vTaskDelay(pdMS_TO_TICKS(1));return s_wakeword_task==nullptr;}
}
extern "C" bool audio_engine_init(void){if(s_initialized)return true;audio_hal_init();if(!wakeword_init())return false;if(wakeword_get_sample_rate()!=16000){wakeword_deinit();return false;}if(!init_audio_pools()){wakeword_deinit();return false;}s_wakeword_detected=false;s_wakeword_running=false;s_conversation_running=false;s_playback_running=false;s_initialized=true;return true;}
extern "C" bool audio_engine_start_wakeword(void){if(!s_initialized||s_wakeword_running||s_conversation_running)return !s_conversation_running;s_wakeword_detected=false;if(audio_hal_start_capture()!=ESP_OK)return false;s_wakeword_running=true;if(xTaskCreate(wakeword_task,"wakeword_task",WAKEWORD_TASK_STACK,nullptr,6,&s_wakeword_task)!=pdPASS){s_wakeword_running=false;audio_hal_stop_capture();return false;}return true;}
extern "C" void audio_engine_stop_wakeword(void){s_wakeword_running=false;audio_hal_stop_capture();}
extern "C" bool audio_engine_process_wakeword(void){if(!s_initialized||!s_wakeword_running)return false;size_t n=0;if(audio_hal_read_pcm(s_pcm_buffer,PCM_BLOCK_SAMPLES,&n)!=ESP_OK||!n)return false;if(wakeword_process_pcm16(s_pcm_buffer,n)){s_wakeword_detected=true;return true;}return false;}
extern "C" bool audio_engine_wakeword_detected(void){return s_wakeword_detected;}
extern "C" void audio_engine_clear_wakeword(void){s_wakeword_detected=false;}
extern "C" void audio_engine_stop(void){audio_engine_stop_wakeword();audio_engine_stop_conversation();audio_engine_stop_playback();}
extern "C" bool audio_engine_start_conversation(void){if(!s_initialized||s_conversation_running)return s_conversation_running;if(!stop_wakeword_and_wait())return false;if(audio_hal_start_capture()!=ESP_OK)return false;xQueueReset(s_mic_free_queue);xQueueReset(s_mic_ready_queue);for(size_t i=0;i<MIC_QUEUE_DEPTH;++i)(void)xQueueSend(s_mic_free_queue,&s_mic_buffers[i],0);s_conversation_running=true;if(xTaskCreate(conversation_task,"audio_conversation",CONVERSATION_TASK_STACK,nullptr,5,&s_conversation_task)!=pdPASS){s_conversation_running=false;audio_hal_stop_capture();s_conversation_task=nullptr;return false;}return true;}
extern "C" void audio_engine_stop_conversation(void){if(!s_conversation_running&&s_conversation_task==nullptr)return;s_conversation_running=false;audio_hal_stop_capture();for(uint32_t i=0;i<200&&s_conversation_task;++i)vTaskDelay(pdMS_TO_TICKS(1));xQueueReset(s_mic_ready_queue);xQueueReset(s_mic_free_queue);for(size_t i=0;i<MIC_QUEUE_DEPTH;++i)(void)xQueueSend(s_mic_free_queue,&s_mic_buffers[i],0);}
extern "C" bool audio_engine_conversation_active(void){return s_conversation_running;}
extern "C" bool audio_engine_read_mic_frame(int16_t *buffer,size_t samples,uint32_t timeout_ms){if(!buffer||samples!=MIC_FRAME_SAMPLES||!s_mic_ready_queue)return false;TickType_t wait=timeout_ms==UINT32_MAX?portMAX_DELAY:pdMS_TO_TICKS(timeout_ms);uint8_t *frame=nullptr;if(xQueueReceive(s_mic_ready_queue,&frame,wait)!=pdTRUE||!frame)return false;memcpy(buffer,frame,MIC_FRAME_BYTES);while(xQueueSend(s_mic_free_queue,&frame,BUFFER_WAIT_TICKS)!=pdTRUE){if(!s_conversation_running)break;}return true;}
extern "C" size_t audio_engine_mic_frame_samples(void){return MIC_FRAME_SAMPLES;}
extern "C" bool audio_engine_start_playback(void){if(!s_initialized)return false;if(s_playback_running)return true;if(audio_hal_start_playback()!=ESP_OK)return false;xQueueReset(s_playback_free_queue);xQueueReset(s_playback_ready_queue);for(size_t i=0;i<PLAYBACK_QUEUE_DEPTH;++i){s_playback_buffers[i]->samples=0;(void)xQueueSend(s_playback_free_queue,&s_playback_buffers[i],0);}s_playback_running=true;s_playback_processing=false;if(xTaskCreate(playback_task,"audio_playback",PLAYBACK_TASK_STACK,nullptr,5,&s_playback_task)!=pdPASS){s_playback_running=false;audio_hal_stop_playback();s_playback_task=nullptr;return false;}return true;}
extern "C" void audio_engine_stop_playback(void){if(!s_playback_running&&s_playback_task==nullptr)return;s_playback_running=false;xQueueReset(s_playback_ready_queue);for(uint32_t i=0;i<200&&s_playback_task;++i)vTaskDelay(pdMS_TO_TICKS(1));xQueueReset(s_playback_free_queue);for(size_t i=0;i<PLAYBACK_QUEUE_DEPTH;++i){s_playback_buffers[i]->samples=0;(void)xQueueSend(s_playback_free_queue,&s_playback_buffers[i],0);}audio_hal_stop_playback();}
extern "C" bool audio_engine_playback_active(void){return s_playback_running;}
extern "C" bool audio_engine_drain_playback(void){if(!s_playback_task)return true;const int64_t deadline=esp_timer_get_time()+15000000LL;while(esp_timer_get_time()<deadline){if(uxQueueMessagesWaiting(s_playback_ready_queue)==0&&!s_playback_processing)break;vTaskDelay(pdMS_TO_TICKS(20));}if(uxQueueMessagesWaiting(s_playback_ready_queue)!=0||s_playback_processing){ESP_LOGE(TAG,"Speaker drain timeout: ready=%u processing=%d",(unsigned)uxQueueMessagesWaiting(s_playback_ready_queue),(int)s_playback_processing);return false;}s_playback_running=false;for(uint32_t i=0;i<200&&s_playback_task;++i)vTaskDelay(pdMS_TO_TICKS(1));xQueueReset(s_playback_free_queue);for(size_t i=0;i<PLAYBACK_QUEUE_DEPTH;++i){s_playback_buffers[i]->samples=0;(void)xQueueSend(s_playback_free_queue,&s_playback_buffers[i],0);}audio_hal_stop_playback();return true;}
extern "C" bool audio_engine_write_speaker_pcm(const int16_t *buffer,size_t samples,uint32_t timeout_ms){if(!buffer||!samples||!s_playback_running||!s_playback_free_queue)return false;TickType_t wait=timeout_ms==UINT32_MAX?portMAX_DELAY:pdMS_TO_TICKS(timeout_ms);size_t offset=0;while(offset<samples&&s_playback_running){PlaybackBlock *block=nullptr;while(xQueueReceive(s_playback_free_queue,&block,wait)!=pdTRUE){if(!s_playback_running)return false;}if(!block)return false;size_t chunk=(samples-offset)>PLAYBACK_BLOCK_SAMPLES?PLAYBACK_BLOCK_SAMPLES: (samples-offset);block->samples=(uint16_t)chunk;memcpy(block->pcm,buffer+offset,chunk*sizeof(int16_t));while(s_playback_running&&xQueueSend(s_playback_ready_queue,&block,wait)!=pdTRUE){}if(!s_playback_running){block->samples=0;(void)xQueueSend(s_playback_free_queue,&block,0);return false;}offset+=chunk;}return offset==samples;}
