/*
 * Sensor Hub: DHT11 -> OLED (LVGL) -> Bluetooth SPP, wired into one pipeline.
 *
 * A sensor task reads the DHT11 every 5s, renders the reading on the OLED,
 * and pushes it over Bluetooth SPP to whatever phone is connected. The BT
 * side handles pairing (legacy PIN + Secure Simple Pairing) and accepts a
 * "GET" command over the link to request an immediate reading on demand.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "lvgl.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#include "dht11.h"
#include "ui.h"

static const char *TAG = "SENSOR_HUB";

/* ---- Verified pin map (see hw_verify/README.md) ---- */
#define DHT11_GPIO      GPIO_NUM_4
#define OLED_SDA_GPIO   21
#define OLED_SCL_GPIO   22

/* ---- OLED / LVGL config ---- */
#define I2C_BUS_PORT            0
#define LCD_PIXEL_CLOCK_HZ      (400 * 1000)
#define LCD_I2C_HW_ADDR         0x3C
#define LCD_H_RES               128
#define LCD_V_RES               64
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8

#define LVGL_TICK_PERIOD_MS     5
#define LVGL_TASK_STACK_SIZE    (4 * 1024)
#define LVGL_TASK_PRIORITY      2
#define LVGL_PALETTE_SIZE       8
#define LVGL_TASK_MAX_DELAY_MS  500
#define LVGL_TASK_MIN_DELAY_MS  (1000 / CONFIG_FREERTOS_HZ)

/* ---- Bluetooth SPP config ---- */
#define BT_LOCAL_DEVICE_NAME    "ESP32_SENSOR_HUB"
#define BT_SPP_SERVER_NAME      "SPP_SERVER"

static uint8_t oled_buffer[LCD_H_RES * LCD_V_RES / 8];
static _lock_t lvgl_api_lock;

/* Guards spp_client_handle, shared between the BT callback task and the sensor task. */
static SemaphoreHandle_t spp_state_mutex;
static uint32_t spp_client_handle = 0; /* 0 == no client connected */

static void bt_send_reading(int temp_c, int humidity, bool valid);

/* ==================== OLED / LVGL plumbing ==================== */

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io_panel,
                                     esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);

    /* Skip LVGL's reserved palette bytes for the monochrome I1 format. */
    px_map += LVGL_PALETTE_SIZE;

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
    int x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            bool chroma_color = (px_map[(hor_res >> 3) * y + (x >> 3)] & 1 << (7 - x % 8));
            uint8_t *buf = oled_buffer + hor_res * (y >> 3) + x;
            if (chroma_color) {
                (*buf) &= ~(1 << (y % 8));
            } else {
                (*buf) |= (1 << (y % 8));
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static lv_display_t *oled_init(void)
{
    ESP_LOGI(TAG, "Init I2C bus (SDA=%d, SCL=%d)", OLED_SDA_GPIO, OLED_SCL_GPIO);
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = LCD_I2C_HW_ADDR,
        .scl_speed_hz = LCD_PIXEL_CLOCK_HZ,
        .transaction_timeout_ms = 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_CMD_BITS,
        .dc_bit_offset = 6,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = LCD_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lv_init();
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(display, panel_handle);

    size_t draw_buffer_sz = LCD_H_RES * LCD_V_RES / 8 + LVGL_PALETTE_SIZE;
    void *buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);

    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    _lock_acquire(&lvgl_api_lock);
    sensor_ui_init(display);
    _lock_release(&lvgl_api_lock);

    return display;
}

/* ==================== Bluetooth SPP + GAP (pairing) ==================== */

static char *bda2str(uint8_t *bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return "";
    }
    uint8_t *p = bda;
    snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static void set_spp_client_handle(uint32_t handle)
{
    xSemaphoreTake(spp_state_mutex, portMAX_DELAY);
    spp_client_handle = handle;
    xSemaphoreGive(spp_state_mutex);

    _lock_acquire(&lvgl_api_lock);
    sensor_ui_set_bt_status(handle != 0);
    _lock_release(&lvgl_api_lock);
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "SPP initialized, starting server '%s'", BT_SPP_SERVER_NAME);
            esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, BT_SPP_SERVER_NAME);
        } else {
            ESP_LOGE(TAG, "SPP init failed, status:%d", param->init.status);
        }
        break;

    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "SPP server started, scn:%d", param->start.scn);
            esp_bt_gap_set_device_name(BT_LOCAL_DEVICE_NAME);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            ESP_LOGI(TAG, "Discoverable as '%s'", BT_LOCAL_DEVICE_NAME);
        } else {
            ESP_LOGE(TAG, "SPP server start failed, status:%d", param->start.status);
        }
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(TAG, "Client connected, handle:%" PRIu32 " addr:[%s]",
                 param->srv_open.handle,
                 bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        set_spp_client_handle(param->srv_open.handle);
        break;

    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "Connection closed, handle:%" PRIu32, param->close.handle);
        set_spp_client_handle(0);
        break;

    case ESP_SPP_DATA_IND_EVT: {
        ESP_LOGI(TAG, "Received %d bytes on handle:%" PRIu32,
                 param->data_ind.len, param->data_ind.handle);

        char cmd[16] = {0};
        int n = MIN(param->data_ind.len, (int)sizeof(cmd) - 1);
        memcpy(cmd, param->data_ind.data, n);
        /* trim trailing CR/LF so "GET\r\n" still matches */
        while (n > 0 && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n')) {
            cmd[--n] = '\0';
        }

        if (strcasecmp(cmd, "GET") == 0) {
            ESP_LOGI(TAG, "Command 'GET' received — sending immediate reading");
            dht11_data_t data;
            esp_err_t ret = dht11_read(DHT11_GPIO, &data);
            bt_send_reading(data.temperature, data.humidity, ret == ESP_OK);
        } else {
            char reply[160];
            int len = snprintf(reply, sizeof(reply), "echo: %.*s\r\n",
                                param->data_ind.len, (char *)param->data_ind.data);
            esp_spp_write(param->data_ind.handle, len, (uint8_t *)reply);
        }
        break;
    }

    case ESP_SPP_WRITE_EVT:
        if (param->write.status != ESP_SPP_SUCCESS) {
            ESP_LOGE(TAG, "SPP write failed, status:%d", param->write.status);
        }
        break;

    default:
        break;
    }
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Pairing succeeded with '%s' [%s]",
                     param->auth_cmpl.device_name,
                     bda2str(param->auth_cmpl.bda, bda_str, sizeof(bda_str)));
        } else {
            ESP_LOGE(TAG, "Pairing failed, status:%d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
        ESP_LOGI(TAG, "Legacy PIN requested, min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
            ESP_LOGI(TAG, "Replying with PIN 1234");
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP numeric comparison, value:%06" PRIu32 " — auto-confirming",
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %06" PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey requested from peer");
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "Link mode changed:%d [%s]", param->mode_chg.mode,
                 bda2str(param->mode_chg.bda, bda_str, sizeof(bda_str)));
        break;

    default:
        break;
    }
}

static void bt_send_reading(int temp_c, int humidity, bool valid)
{
    uint32_t handle;
    xSemaphoreTake(spp_state_mutex, portMAX_DELAY);
    handle = spp_client_handle;
    xSemaphoreGive(spp_state_mutex);

    if (handle == 0) {
        return;
    }

    char msg[64];
    int len = valid
        ? snprintf(msg, sizeof(msg), "T:%dC H:%d%%\r\n", temp_c, humidity)
        : snprintf(msg, sizeof(msg), "SENSOR_ERROR\r\n");
    esp_spp_write(handle, len, (uint8_t *)msg);
}

static void bt_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&bt_spp_cfg));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    char bda_str[18] = {0};
    ESP_LOGI(TAG, "Own BT address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
}

/* ==================== Sensor pipeline ==================== */

/* DHT11's bit-banged timing occasionally loses a race with the Bluetooth
 * radio's interrupt activity (a single dropped read, always on the last
 * bit) — that's expected and recovers on its own. Only treat the sensor as
 * actually erroring out after several reads in a row fail, and otherwise
 * keep showing/broadcasting the last known-good reading instead of
 * flashing "sensor error" on every transient miss. */
#define SENSOR_MAX_CONSECUTIVE_FAILURES 3

static void sensor_task(void *arg)
{
    dht11_data_t data;
    dht11_data_t last_good = {0};
    bool have_good = false;
    int consecutive_failures = 0;

    while (1) {
        esp_err_t ret = dht11_read(DHT11_GPIO, &data);

        if (ret == ESP_OK) {
            consecutive_failures = 0;
            last_good = data;
            have_good = true;
            ESP_LOGI(TAG, "DHT11: %dC %d%%RH", data.temperature, data.humidity);
        } else {
            consecutive_failures++;
            ESP_LOGW(TAG, "DHT11 read failed (%d in a row): %s",
                     consecutive_failures, esp_err_to_name(ret));
        }

        bool valid = have_good && consecutive_failures < SENSOR_MAX_CONSECUTIVE_FAILURES;

        uint32_t handle;
        xSemaphoreTake(spp_state_mutex, portMAX_DELAY);
        handle = spp_client_handle;
        xSemaphoreGive(spp_state_mutex);

        _lock_acquire(&lvgl_api_lock);
        sensor_ui_update(last_good.temperature, last_good.humidity, valid, handle != 0);
        _lock_release(&lvgl_api_lock);

        bt_send_reading(last_good.temperature, last_good.humidity, valid);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sensor Hub: DHT11 -> OLED -> Bluetooth SPP ===");

    spp_state_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(dht11_init(DHT11_GPIO));
    oled_init();
    bt_init();

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}
