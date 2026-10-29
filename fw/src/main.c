#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "MIC_TEST";

// ---- 1. Settings: where the mic is wired, and the agreed audio format ----
#define MIC_SCK_PIN GPIO_NUM_4 // bit clock  (BCLK)
#define MIC_WS_PIN GPIO_NUM_5  // word select (left/right clock)
#define MIC_SD_PIN GPIO_NUM_6  // data coming out of the mic

#define SAMPLE_RATE_HZ 16000                                 // 16 kHz
#define FRAME_MS 20                                          // 20 ms chunks
#define SAMPLES_PER_FRAME (SAMPLE_RATE_HZ * FRAME_MS / 1000) // = 320

// ---- 2. Storage ----
static i2s_chan_handle_t rx_chan;      // "handle" = our ticket to the I2S channel
static int32_t raw[SAMPLES_PER_FRAME]; // one 20 ms chunk of raw mic samples

// ---- 3. Set up I2S for the INMP441 ----
static void mic_init(void)
{
    // a) Ask ESP-IDF for an I2S channel. The ESP is the "master": it makes the clock.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan)); // NULL = no transmit, receive only

    // b) Describe the clock speed, the sample format and the pins
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED, // INMP441 doesn't need a master clock
                .bclk = MIC_SCK_PIN,
                .ws = MIC_WS_PIN,
                .dout = I2S_GPIO_UNUSED, // we're not sending audio out
                .din = MIC_SD_PIN,
                .invert_flags = {0},
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // L/R is tied to GND -> left channel

    // c) Apply the settings and switch the channel on
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}

// ---- 4. Main program: read chunks forever and print how loud they are ----
void app_main(void)
{
    mic_init();
    ESP_LOGI(TAG, "Mic started: %d Hz, %d samples per frame", SAMPLE_RATE_HZ, SAMPLES_PER_FRAME);

    int frame_count = 0;

    while (1)
    {
        // a) Wait for one 20 ms chunk from the mic
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_chan, raw, sizeof(raw), &bytes_read, portMAX_DELAY);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(err));
            continue;
        }

        // b) Convert each sample to 16-bit and find the quietest and loudest points
        int n = bytes_read / sizeof(int32_t);
        int16_t min = INT16_MAX, max = INT16_MIN;
        for (int i = 0; i < n; i++)
        {
            int16_t s = (int16_t)(raw[i] >> 16); // keep the top 16 bits
            if (s < min) min = s;
            if (s > max) max = s;
        }
        int level = max - min; // "peak-to-peak" = how big the sound wave is

        // c) Print every 5th chunk (10 lines/second) so the monitor isn't flooded
        if (++frame_count % 5 == 0)
        {
            char bar[41];
            int len = level / 200; // scale the level into a bar
            if (len > 40) len = 40;
            for (int i = 0; i < len; i++)
                bar[i] = '#';
            bar[len] = '\0';
            printf("level %6d  min %6d  max %6d  |%s\n", level, min, max, bar);
        }
    }
}