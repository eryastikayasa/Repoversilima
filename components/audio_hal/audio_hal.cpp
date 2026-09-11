#include "audio_hal.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_HAL";

static constexpr gpio_num_t MIC_I2S_SCK = GPIO_NUM_5;
static constexpr gpio_num_t MIC_I2S_WS  = GPIO_NUM_4;
static constexpr gpio_num_t MIC_I2S_SD  = GPIO_NUM_6;
static constexpr gpio_num_t SPK_I2S_BCLK = GPIO_NUM_15;
static constexpr gpio_num_t SPK_I2S_LRCK = GPIO_NUM_16;
static constexpr gpio_num_t SPK_I2S_DOUT = GPIO_NUM_7;

static constexpr uint32_t MIC_SAMPLE_RATE = 16000;
static constexpr uint32_t SPK_SAMPLE_RATE = 24000;

static i2s_chan_handle_t s_rx = nullptr;
static i2s_chan_handle_t s_tx = nullptr;
static bool s_initialized = false;
static bool s_capture_started = false;
static bool s_playback_started = false;

// Keep the temporary I2S conversion buffers out of task stacks.
// 1024 x 32-bit = 4096 bytes each.
static int32_t s_rx_raw[1024];
static int32_t s_tx_raw[1024];

void audio_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Audio HAL sudah diinisialisasi");
        return;
    }

    ESP_LOGI(TAG, "Audio HAL init: MIC=16k PCM16, SPK=24k PCM16");

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 6;
    tx_chan_cfg.dma_frame_num = 240;
    tx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &s_tx, nullptr));

    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6;
    rx_chan_cfg.dma_frame_num = 240;
    rx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &s_rx));

    i2s_std_config_t rx_cfg = {};
    rx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    rx_cfg.gpio_cfg.bclk = MIC_I2S_SCK;
    rx_cfg.gpio_cfg.ws = MIC_I2S_WS;
    rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_cfg.gpio_cfg.din = MIC_I2S_SD;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_cfg));

    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
    tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    // Match the proven Repo4 speaker configuration exactly.
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    tx_cfg.slot_cfg.ws_pol = false;
    tx_cfg.slot_cfg.bit_shift = true;
    tx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    tx_cfg.gpio_cfg.bclk = SPK_I2S_BCLK;
    tx_cfg.gpio_cfg.ws = SPK_I2S_LRCK;
    tx_cfg.gpio_cfg.dout = SPK_I2S_DOUT;
    tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_cfg));

    s_initialized = true;
    ESP_LOGI(TAG, "Audio HAL hardware siap; I2S RX/TX belum dimulai");
}

esp_err_t audio_hal_start_capture(void)
{
    if (!s_initialized || !s_rx) return ESP_ERR_INVALID_STATE;
    if (s_capture_started) return ESP_OK;

    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) {
        s_capture_started = true;
        ESP_LOGI(TAG, "MIC capture START");
    }
    return err;
}

esp_err_t audio_hal_stop_capture(void)
{
    if (!s_initialized || !s_rx) return ESP_ERR_INVALID_STATE;
    if (!s_capture_started) return ESP_OK;

    esp_err_t err = i2s_channel_disable(s_rx);
    if (err == ESP_OK) {
        s_capture_started = false;
        ESP_LOGI(TAG, "MIC capture STOP");
    }
    return err;
}

esp_err_t audio_hal_read_pcm(int16_t *buffer, size_t samples, size_t *samples_read)
{
    if (samples_read) *samples_read = 0;
    if (!buffer || samples == 0 || !samples_read) return ESP_ERR_INVALID_ARG;
    if (!s_capture_started || !s_rx) return ESP_ERR_INVALID_STATE;
    if (samples > 1024) return ESP_ERR_INVALID_SIZE;

    size_t bytes_read = 0;
    const size_t input_bytes = samples * sizeof(int32_t);

    esp_err_t err = i2s_channel_read(
        s_rx, s_rx_raw, input_bytes, &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) return err;

    const size_t count = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
        buffer[i] = (int16_t)(s_rx_raw[i] >> 16);
    }
    *samples_read = count;
    return ESP_OK;
}

esp_err_t audio_hal_start_playback(void)
{
    if (!s_initialized || !s_tx) return ESP_ERR_INVALID_STATE;
    if (s_playback_started) return ESP_OK;

    esp_err_t err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        s_playback_started = true;
        ESP_LOGI(TAG, "SPK playback START");
    }
    return err;
}

esp_err_t audio_hal_stop_playback(void)
{
    if (!s_initialized || !s_tx) return ESP_ERR_INVALID_STATE;
    if (!s_playback_started) return ESP_OK;

    esp_err_t err = i2s_channel_disable(s_tx);
    if (err == ESP_OK) {
        s_playback_started = false;
        ESP_LOGI(TAG, "SPK playback STOP");
    }
    return err;
}

esp_err_t audio_hal_write_pcm(const int16_t *buffer, size_t samples, size_t *samples_written)
{
    if (samples_written) *samples_written = 0;
    if (!buffer || samples == 0 || !samples_written) return ESP_ERR_INVALID_ARG;
    if (!s_playback_started || !s_tx) return ESP_ERR_INVALID_STATE;
    if (samples > 1024) return ESP_ERR_INVALID_SIZE;

    // Speaker I2S is configured as 32-bit. Expand PCM16 to the 32-bit
    // left-justified representation used by the proven Repo4 path.
    for (size_t i = 0; i < samples; ++i) {
        s_tx_raw[i] = ((int32_t)buffer[i]) << 16;
    }

    // The DMA configuration uses 240-sample frames. Keep each I2S write at
    // one DMA frame, matching the proven Repo4 speaker path. This avoids
    // relying on larger driver transfers while preserving partial-write
    // continuation when the DMA ring accepts less than one frame.
    constexpr size_t I2S_WRITE_SAMPLES = 240;
    constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;

    size_t total_written = 0;
    while (total_written < samples) {
        const size_t remaining = samples - total_written;
        const size_t chunk = remaining > I2S_WRITE_SAMPLES
                           ? I2S_WRITE_SAMPLES
                           : remaining;

        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(
            s_tx,
            s_tx_raw + total_written,
            chunk * sizeof(int32_t),
            &bytes_written,
            pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));

        size_t chunk_written = bytes_written / sizeof(int32_t);
        if (chunk_written > chunk) chunk_written = chunk;
        total_written += chunk_written;

        if (err != ESP_OK || chunk_written != chunk) {
            *samples_written = total_written;
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "I2S speaker write fail: err=%s written=%u/%u",
                         esp_err_to_name(err),
                         (unsigned)bytes_written,
                         (unsigned)(chunk * sizeof(int32_t)));
                vTaskDelay(pdMS_TO_TICKS(1));
                return err;
            }

            // No progress without an error is treated as a bounded failure.
            ESP_LOGW(TAG,
                     "I2S speaker write tidak lengkap: written=%u/%u",
                     (unsigned)bytes_written,
                     (unsigned)(chunk * sizeof(int32_t)));
            return ESP_ERR_TIMEOUT;
        }
    }

    *samples_written = total_written;
    return ESP_OK;
}
