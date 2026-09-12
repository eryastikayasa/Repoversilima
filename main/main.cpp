#include "audio_engine.h"
#include "audio_hal.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== RepoVersilima clean architecture boot ===");

    // Proven hardware/system components remain external to the new data path.
    audio_hal_init();
    wifi_init_sta();

    if (!wifi_wait_for_connection(30000)) {
        ESP_LOGW(TAG, "WiFi not ready; application stays alive");
    }

    if (!audio_engine_init()) {
        ESP_LOGE(TAG, "Audio Engine init failed");
        return;
    }

    ESP_LOGI(TAG, "System foundation READY");
    ESP_LOGI(TAG, "MIC -> Audio Engine -> WebSocket -> Gemini");
    ESP_LOGI(TAG, "Gemini -> WebSocket -> Audio Engine -> Speaker");

    // Gemini transport is intentionally not started here yet.
    // The URI/session configuration will be supplied by the web-config layer.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
