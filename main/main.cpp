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
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";
static constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;

static bool init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS perlu di-erase lalu init ulang");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase gagal: %s", esp_err_to_name(err));
            return false;
        }
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init gagal: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "NVS READY");
    return true;
}

static void init_display(void)
{
    display_face_init();
    display_text_init();
    display_text_set_status("Memulai...");
    display_engine_init();
    display_engine_start();

    ESP_LOGI(TAG, "DISPLAY READY");
}

static bool init_boot_button(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO0 BOOT button init gagal: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "BOOT button READY: GPIO0, tekan untuk memulai sesi Gemini");
    return true;
}

static bool init_wakeword(void)
{
    if (!audio_engine_init()) {
        ESP_LOGE(TAG, "AudioEngine/WakeWord init gagal");
        return false;
    }

    if (!audio_engine_start_wakeword()) {
        ESP_LOGE(TAG, "AudioEngine/WakeWord start gagal");
        audio_engine_stop();
        return false;
    }

    display_face_set_state(FACE_IDLE);
    display_text_set_status("Siap - ucap HI ESP");
    ESP_LOGI(TAG, "WAKEWORD READY - menunggu HI, ESP / BOOT");
    return true;
}

static bool init_websocket(void)
{
    const esp_err_t err = websocket_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket init gagal: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "WEBSOCKET READY - menunggu WakeWord / BOOT");
    return true;
}

static bool restart_wakeword_after_conversation_failure(void)
{
    websocket_disconnect();

    if (!audio_engine_start_wakeword()) {
        ESP_LOGE(TAG, "Gagal mengembalikan WakeWord setelah conversation gagal");
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WakeWord gagal");
        return false;
    }

    display_face_set_state(FACE_IDLE);
    display_text_set_status("Siap - ucap HI ESP");
    return true;
}

static bool start_conversation(void)
{
    display_face_set_state(FACE_LISTENING);
    display_text_set_status("Menghubungkan Gemini...");

    // websocket_connect() starts the client asynchronously. Do not interpret
    // the initial NOT-CONNECTED state as a failure; wait for the CONNECTED
    // event while keeping the Gemini setupComplete gate below.
    const esp_err_t ws_err = websocket_connect();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket connect gagal: %s", esp_err_to_name(ws_err));
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Gemini gagal");
        restart_wakeword_after_conversation_failure();
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (!websocket_event_gemini_ready()) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGE(TAG, "Timeout menunggu Gemini setupComplete");
            display_face_set_state(FACE_ERROR);
            display_text_set_status("Gemini timeout");
            restart_wakeword_after_conversation_failure();
            return false;
        }

        if (!websocket_is_connected()) {
            ESP_LOGD(TAG, "Menunggu WebSocket CONNECTED/Gemini setupComplete...");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    display_text_set_status("Mendengarkan...");

    if (!audio_engine_start_conversation()) {
        ESP_LOGE(TAG, "Conversation AudioEngine start gagal");
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Audio gagal");
        restart_wakeword_after_conversation_failure();
        return false;
    }

    if (!websocket_audio_start()) {
        ESP_LOGE(TAG, "WebSocket audio uplink start gagal");
        audio_engine_stop_conversation();
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Uplink gagal");
        restart_wakeword_after_conversation_failure();
        return false;
    }

    ESP_LOGI(TAG, "CONVERSATION START: AudioEngine -> WebSocket -> Gemini");
    return true;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 application start");
    ESP_LOGI(TAG, "========================================");

    if (!init_nvs()) {
        ESP_LOGE(TAG, "NVS gagal - hentikan startup");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (web_config_is_needed()) {
        ESP_LOGW(TAG, "Konfigurasi belum siap - masuk WebConfig");
        web_config_start();

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "Konfigurasi ditemukan - lanjut Wi-Fi");

    init_display();
    display_text_set_status("WiFi...");

    wifi_init_sta();

    if (!wifi_wait_for_connection(30000)) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WiFi gagal");
        ESP_LOGE(TAG, "Wi-Fi belum READY setelah timeout");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    display_face_set_state(FACE_IDLE);
    display_text_set_status("WiFi OK");
    ESP_LOGI(TAG, "WIFI READY");

    if (!init_websocket()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WebSocket gagal");
        ESP_LOGE(TAG, "WebSocket belum READY - hentikan startup");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!init_boot_button()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("BOOT gagal");
        ESP_LOGE(TAG, "BOOT button belum READY - hentikan startup");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!init_wakeword()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WakeWord gagal");
        ESP_LOGE(TAG, "WakeWord belum READY - hentikan startup");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    bool boot_button_down = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);

    while (true) {
        const bool boot_pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);

        if (boot_pressed && !boot_button_down) {
            vTaskDelay(pdMS_TO_TICKS(30));

            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "MAIN: BOOT button event");

                if (start_conversation()) {
                    ESP_LOGI(TAG, "MAIN: conversation mode ACTIVE (BOOT)");
                }

                boot_button_down = true;
            }
        } else if (!boot_pressed) {
            boot_button_down = false;
        }

        if (audio_engine_wakeword_detected()) {
            ESP_LOGI(TAG, "MAIN: WakeWord event");
            audio_engine_clear_wakeword();

            if (start_conversation()) {
                ESP_LOGI(TAG, "MAIN: conversation mode ACTIVE (WakeWord)");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
