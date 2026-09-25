#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "esp_hf_client_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "bt_app_av.h"
#include "oled_status.h"
#include "touch_ctl.h"

static const char *TAG = "TOUCH";

#define TOUCH_CHAN        4        /* T4 = GPIO13 */
#define POLL_MS           20
#define PRESS_RATIO       0.85f    /* touched when the reading drops below this x baseline */
#define RELEASE_RATIO     0.92f    /* and released once it climbs back above this */
#define DEBOUNCE_POLLS    2        /* consecutive polls to accept a press or release */
#define LONG_PRESS_MS     1000

static touch_channel_handle_t s_chan;

static bool touch_hw_init(void)
{
    touch_sensor_handle_t sens;
    touch_sensor_sample_config_t sample_cfg[] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7),
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    /* Threshold 0 never triggers the driver's own callbacks; this module
     * polls the filtered reading and does its own thresholds instead. */
    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = {0},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();

    return touch_sensor_new_controller(&sens_cfg, &sens) == ESP_OK &&
           touch_sensor_new_channel(sens, TOUCH_CHAN, &chan_cfg, &s_chan) == ESP_OK &&
           touch_sensor_config_filter(sens, &filter_cfg) == ESP_OK &&
           touch_sensor_enable(sens) == ESP_OK &&
           touch_sensor_start_continuous_scanning(sens) == ESP_OK;
}

static uint32_t touch_read(void)
{
    uint32_t v = 0;
    touch_channel_read_data(s_chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, &v);
    return v;
}

static void on_gesture(bool long_press)
{
    const char *what;
    if (g_stat_ringing) {
        if (long_press) {
            esp_hf_client_reject_call();
            what = "reject call";
        } else {
            esp_hf_client_answer_call();
            what = "answer call";
        }
    } else if (g_stat_in_call || g_stat_outgoing || g_stat_audio) {
        esp_hf_client_reject_call();    /* AT+CHUP also ends an active call */
        what = "hang up";
    } else if (!long_press) {
        bool playing = g_stat_music_tick &&
                       xTaskGetTickCount() - g_stat_music_tick < pdMS_TO_TICKS(1000);
        bt_app_av_passthrough(playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
        what = playing ? "pause music" : "play music";
    } else {
        what = "nothing (long press, no call)";
    }
    ESP_LOGI(TAG, "%s -> %s", long_press ? "long press" : "tap", what);
}

static void touch_task(void *arg)
{
    /* Baseline: the untouched reading, averaged. Touch lowers the reading. */
    vTaskDelay(pdMS_TO_TICKS(300));
    float baseline = 0;
    for (int i = 0; i < 20; i++) {
        baseline += touch_read();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    baseline /= 20;
    ESP_LOGI(TAG, "pad on GPIO13 ready, baseline %.0f (press below %.0f)",
             baseline, baseline * PRESS_RATIO);

    bool pressed = false, long_fired = false;
    int streak = 0;
    TickType_t press_start = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        float v = touch_read();
        bool below = v < baseline * PRESS_RATIO;
        bool above = v > baseline * RELEASE_RATIO;

        if (!pressed) {
            streak = below ? streak + 1 : 0;
            if (streak >= DEBOUNCE_POLLS) {
                pressed = true;
                long_fired = false;
                streak = 0;
                press_start = xTaskGetTickCount();
            } else if (!below) {
                /* follow slow drift (temperature, humidity) while untouched */
                baseline += (v - baseline) * 0.01f;
            }
        } else {
            if (!long_fired && xTaskGetTickCount() - press_start >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                long_fired = true;          /* fire while still held */
                on_gesture(true);
            }
            streak = above ? streak + 1 : 0;
            if (streak >= DEBOUNCE_POLLS) {
                pressed = false;
                streak = 0;
                if (!long_fired) {
                    on_gesture(false);
                }
            }
        }
    }
}

void touch_ctl_start(void)
{
    if (!touch_hw_init()) {
        ESP_LOGE(TAG, "touch sensor init failed");
        return;
    }
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 4, NULL, 1);
}
