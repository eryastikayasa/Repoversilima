#include "web_config.h"
#include "wifi_manager.h"

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

    wifi_init_sta();

    if (!wifi_wait_for_connection(30000)) {
        ESP_LOGE(TAG, "Wi-Fi belum READY setelah timeout");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "WIFI READY - tahap dasar selesai");

    // Subsystem berikutnya akan ditambahkan bertahap.
    // Jangan start Display/WakeWord/Audio/WebSocket/Gemini di tahap ini.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
