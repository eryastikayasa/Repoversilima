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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

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
    ESP_LOGI(TAG, "WAKEWORD READY - menunggu HI, ESP");
    return true;
}

static bool init_websocket(void)
{
    const esp_err_t err = websocket_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket init gagal: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "WEBSOCKET READY - menunggu WakeWord");
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

    // Connect first. AudioEngine must not start consuming MIC into a bounded
    // queue while the WebSocket/TLS/Gemini setup handshake is still pending.
    const esp_err_t ws_err = websocket_connect();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket connect gagal: %s", esp_err_to_name(ws_err));
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Gemini gagal");
        restart_wakeword_after_conversation_failure();
        return false;
    }

    // Gemini setupComplete is the explicit gate before audio uplink begins.
    // Give TLS + Gemini handshake enough time without involving AudioEngine.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (!websocket_event_gemini_ready()) {
        if (!websocket_is_connected()) {
            ESP_LOGE(TAG, "WebSocket putus sebelum Gemini setupComplete");
            display_face_set_state(FACE_ERROR);
            display_text_set_status("Gemini putus");
            restart_wakeword_after_conversation_failure();
            return false;
        }

        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGE(TAG, "Timeout menunggu Gemini setupComplete");
            display_face_set_state(FACE_ERROR);
            display_text_set_status("Gemini timeout");
            restart_wakeword_after_conversation_failure();
            return false;
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

    // WebConfig menjadi gerbang konfigurasi pertama.
    // Jika SSID/API key belum tersedia, perangkat masuk AP Config Mode
    // dan tidak melanjutkan ke subsystem berikutnya.
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

    if (!init_wakeword()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WakeWord gagal");
        ESP_LOGE(TAG, "WakeWord belum READY - hentikan startup");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Main hanya bertindak sebagai supervisor event tingkat aplikasi.
    // Pemrosesan mic dan WakeNet berjalan di dalam AudioEngine.
    while (true) {
        if (audio_engine_wakeword_detected()) {
            ESP_LOGI(TAG, "MAIN: WakeWord event");
            audio_engine_clear_wakeword();

            if (start_conversation()) {
                // Conversation mode mengambil alih MIC. WakeWord sudah
                // dihentikan oleh AudioEngine sebelum ownership berpindah.
                ESP_LOGI(TAG, "MAIN: conversation mode ACTIVE");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
