#include "web_config.h"
#include "wifi_manager.h"
#include "display_engine.h"
#include "display_face.h"
#include "display_text.h"
#include "audio_engine.h"
#include "websocket.h"
#include "websocket_audio.h"
#include "websocket_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";
static constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;
static constexpr int64_t SESSION_INACTIVITY_US = 60000000LL;

static bool init_nvs(void){esp_err_t err=nvs_flash_init();if(err==ESP_ERR_NVS_NO_FREE_PAGES||err==ESP_ERR_NVS_NEW_VERSION_FOUND){err=nvs_flash_erase();if(err!=ESP_OK)return false;err=nvs_flash_init();}if(err!=ESP_OK)return false;return true;}
static void init_display(void){display_face_init();display_text_init();display_text_set_status("Memulai...");display_engine_init();display_engine_start();}
static bool init_boot_button(void){gpio_config_t io={};io.pin_bit_mask=1ULL<<BOOT_BUTTON_GPIO;io.mode=GPIO_MODE_INPUT;io.pull_up_en=GPIO_PULLUP_ENABLE;io.pull_down_en=GPIO_PULLDOWN_DISABLE;io.intr_type=GPIO_INTR_DISABLE;return gpio_config(&io)==ESP_OK;}
static bool init_wakeword(void){if(!audio_engine_init())return false;if(!audio_engine_start_wakeword()){audio_engine_stop();return false;}display_face_set_state(FACE_IDLE);display_text_set_status("Siap - ucap HI ESP");return true;}
static bool init_websocket(void){return websocket_init()==ESP_OK;}
static bool restart_wakeword_after_conversation_failure(void){websocket_disconnect();if(!audio_engine_start_wakeword()){display_face_set_state(FACE_ERROR);display_text_set_status("WakeWord gagal");return false;}display_face_set_state(FACE_IDLE);display_text_set_status("Siap - ucap HI ESP");return true;}
static bool start_conversation(void){display_face_set_state(FACE_LISTENING);display_text_set_status("Menghubungkan Gemini...");if(websocket_connect()!=ESP_OK){restart_wakeword_after_conversation_failure();return false;}const TickType_t deadline=xTaskGetTickCount()+pdMS_TO_TICKS(15000);while(!websocket_event_gemini_ready()){if((int32_t)(xTaskGetTickCount()-deadline)>=0){display_face_set_state(FACE_ERROR);display_text_set_status("Gemini timeout");restart_wakeword_after_conversation_failure();return false;}vTaskDelay(pdMS_TO_TICKS(20));}display_text_set_status("Mendengarkan...");if(!audio_engine_start_conversation()){restart_wakeword_after_conversation_failure();return false;}if(!websocket_audio_start()){audio_engine_stop_conversation();restart_wakeword_after_conversation_failure();return false;}ESP_LOGI(TAG,"CONVERSATION START: AudioEngine -> WebSocket -> Gemini");return true;}

static bool finish_conversation_lossless(void)
{
    ESP_LOGI(TAG,"SESSION TIMEOUT: 60s tanpa aktivitas Gemini/MIC -> drain audio");
    display_text_set_status("Menyelesaikan audio...");

    // First stop MIC production. No new PCM may enter TX while draining.
    audio_engine_stop_conversation();

    const bool tx_ok=websocket_audio_drain_stop();
    const bool rx_ok=websocket_event_drain();
    const bool playback_ok=audio_engine_drain_playback();

    if(!tx_ok||!rx_ok||!playback_ok){
        ESP_LOGE(TAG,"SESSION DRAIN GAGAL: TX=%d RX=%d PLAYBACK=%d; tidak mengklaim audio lengkap",(int)tx_ok,(int)rx_ok,(int)playback_ok);
    }

    websocket_disconnect();
    display_face_set_state(FACE_IDLE);
    display_text_set_status("Siap - ucap HI ESP");
    return tx_ok&&rx_ok&&playback_ok;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG,"========================================");
    ESP_LOGI(TAG,"ESP32-S3 application start");
    ESP_LOGI(TAG,"========================================");
    if(!init_nvs())while(true)vTaskDelay(pdMS_TO_TICKS(1000));
    if(web_config_is_needed()){web_config_start();while(true)vTaskDelay(pdMS_TO_TICKS(1000));}
    init_display();display_text_set_status("WiFi...");wifi_init_sta();
    if(!wifi_wait_for_connection(30000)){display_face_set_state(FACE_ERROR);display_text_set_status("WiFi gagal");while(true)vTaskDelay(pdMS_TO_TICKS(1000));}
    display_face_set_state(FACE_IDLE);display_text_set_status("WiFi OK");
    if(!init_websocket()){display_face_set_state(FACE_ERROR);display_text_set_status("WebSocket gagal");while(true)vTaskDelay(pdMS_TO_TICKS(1000));}
    if(!init_boot_button()){display_face_set_state(FACE_ERROR);display_text_set_status("BOOT gagal");while(true)vTaskDelay(pdMS_TO_TICKS(1000));}
    if(!init_wakeword()){display_face_set_state(FACE_ERROR);display_text_set_status("WakeWord gagal");while(true)vTaskDelay(pdMS_TO_TICKS(1000));}

    bool boot_button_down=(gpio_get_level(BOOT_BUTTON_GPIO)==0);
    bool session_active=false;
    while(true){
        const bool boot_pressed=(gpio_get_level(BOOT_BUTTON_GPIO)==0);
        if(boot_pressed&&!boot_button_down){vTaskDelay(pdMS_TO_TICKS(30));if(gpio_get_level(BOOT_BUTTON_GPIO)==0){if(start_conversation())session_active=true;boot_button_down=true;}}
        else if(!boot_pressed)boot_button_down=false;

        if(!session_active&&audio_engine_wakeword_detected()){audio_engine_clear_wakeword();if(start_conversation())session_active=true;}

        if(session_active){
            const int64_t last=websocket_event_last_activity_us();
            if(last>0&&esp_timer_get_time()-last>=SESSION_INACTIVITY_US){finish_conversation_lossless();session_active=false;}
            else if(!websocket_audio_running()&&!audio_engine_conversation_active())session_active=false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
