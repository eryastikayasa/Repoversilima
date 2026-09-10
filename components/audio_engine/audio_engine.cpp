#include "audio_engine.h"

#include "audio_hal.h"
#include "wakeword.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "AUDIO_ENGINE";
static constexpr size_t PCM_BLOCK_SAMPLES = 512;
static constexpr uint32_t WAKEWORD_TASK_DELAY_MS = 1;
// WakeNet processing and ESP-SR calls need more headroom than the original
// 4 KB task. Keep the audio buffers static so this stack is for call depth.
static constexpr uint32_t WAKEWORD_TASK_STACK = 8192;

static int16_t s_pcm_buffer[PCM_BLOCK_SAMPLES];
static TaskHandle_t s_wakeword_task = nullptr;
static bool s_initialized = false;
static volatile bool s_wakeword_running = false;
static volatile bool s_wakeword_detected = false;

static void wakeword_task(void *)
{
    while (s_wakeword_running) {
        if (audio_engine_process_wakeword()) {
            ESP_LOGI(TAG, "WakeWord event diterima AudioEngine");
        }

        vTaskDelay(pdMS_TO_TICKS(WAKEWORD_TASK_DELAY_MS));
    }

    s_wakeword_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" bool audio_engine_init(void)
{
    if (s_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing AudioEngine");

    audio_hal_init();

    if (!wakeword_init()) {
        ESP_LOGE(TAG, "WakeWord initialization failed");
        return false;
    }

    if (wakeword_get_sample_rate() != 16000) {
        ESP_LOGE(TAG, "WakeWord sample rate mismatch: %dHz",
                 wakeword_get_sample_rate());
        wakeword_deinit();
        return false;
    }

    s_wakeword_detected = false;
    s_wakeword_running = false;
    s_wakeword_task = nullptr;
    s_initialized = true;

    ESP_LOGI(TAG, "AudioEngine ready: MIC -> Audio HAL -> WakeNet");
    return true;
}

extern "C" bool audio_engine_start_wakeword(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "AudioEngine belum diinisialisasi");
        return false;
    }

    if (s_wakeword_running) {
        return true;
    }

    s_wakeword_detected = false;

    const esp_err_t err = audio_hal_start_capture();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start MIC capture: %s", esp_err_to_name(err));
        return false;
    }

    s_wakeword_running = true;

    const BaseType_t result = xTaskCreate(
        wakeword_task,
        "wakeword_task",
        WAKEWORD_TASK_STACK,
        nullptr,
        6,
        &s_wakeword_task
    );

    if (result != pdPASS) {
        s_wakeword_running = false;
        audio_hal_stop_capture();
        ESP_LOGE(TAG, "Gagal membuat WakeWord task");
        return false;
    }

    ESP_LOGI(TAG, "WakeWord capture START");
    return true;
}

extern "C" void audio_engine_stop_wakeword(void)
{
    if (!s_wakeword_running) {
        return;
    }

    s_wakeword_running = false;
    audio_hal_stop_capture();
    ESP_LOGI(TAG, "WakeWord capture STOP");
}

extern "C" bool audio_engine_process_wakeword(void)
{
    if (!s_initialized || !s_wakeword_running) {
        return false;
    }

    size_t samples_read = 0;
    const esp_err_t err = audio_hal_read_pcm(
        s_pcm_buffer,
        PCM_BLOCK_SAMPLES,
        &samples_read);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio HAL read gagal: %s", esp_err_to_name(err));
        return false;
    }

    if (samples_read == 0) {
        return false;
    }

    if (wakeword_process_pcm16(s_pcm_buffer, samples_read)) {
        s_wakeword_detected = true;
        return true;
    }

    return false;
}

extern "C" bool audio_engine_wakeword_detected(void)
{
    return s_wakeword_detected;
}

extern "C" void audio_engine_clear_wakeword(void)
{
    s_wakeword_detected = false;
}

extern "C" void audio_engine_stop(void)
{
    audio_engine_stop_wakeword();
}
