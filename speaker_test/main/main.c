/* Speaker check: loops test tones out of the DAC on GPIO25, using the same
 * DAC setup as hfp_mic_test (channel 0, 16kHz, APLL clock), so a pass here
 * means the amp/speaker wiring is good for calls too. */
#include <math.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/dac_continuous.h"
#include "esp_log.h"

#define TAG "spk_test"

#define SAMPLE_RATE 16000
#define CHUNK       256
/* Peak swing around mid-scale (128). Keep modest: a PAM8403 is loud. */
#define AMPLITUDE   60

static dac_continuous_handle_t s_dac;

/* Plays a tone gliding from f_start to f_end Hz (equal for a steady tone);
 * amplitude 0 plays silence. */
static void play(float f_start, float f_end, int amplitude, int ms)
{
    uint8_t buf[CHUNK];
    int total = SAMPLE_RATE * ms / 1000;
    static float phase = 0;

    for (int done = 0; done < total; done += CHUNK) {
        for (int i = 0; i < CHUNK; i++) {
            float f = f_start + (f_end - f_start) * (done + i) / total;
            phase += 2.0f * (float)M_PI * f / SAMPLE_RATE;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
            buf[i] = (uint8_t)(128 + amplitude * sinf(phase));
        }
        dac_continuous_write(s_dac, buf, CHUNK, NULL, -1);
    }
}

void app_main(void)
{
    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,  /* GPIO25 */
        .desc_num = 4,
        .buf_size = CHUNK,
        .freq_hz = SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cfg, &s_dac));
    ESP_ERROR_CHECK(dac_continuous_enable(s_dac));
    ESP_LOGI(TAG, "DAC ready on GPIO25 at %d Hz", SAMPLE_RATE);

    for (int round = 1;; round++) {
        ESP_LOGI(TAG, "round %d: 440 Hz tone (2 s)", round);
        play(440, 440, AMPLITUDE, 2000);
        ESP_LOGI(TAG, "round %d: 1 kHz tone (2 s)", round);
        play(1000, 1000, AMPLITUDE, 2000);
        ESP_LOGI(TAG, "round %d: silence (1 s)", round);
        play(0, 0, 0, 1000);
        ESP_LOGI(TAG, "round %d: sweep 200 Hz -> 4 kHz (3 s)", round);
        play(200, 4000, AMPLITUDE, 3000);
        ESP_LOGI(TAG, "round %d: silence (1 s)", round);
        play(0, 0, 0, 1000);
    }
}
