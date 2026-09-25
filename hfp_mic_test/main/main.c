/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_pbac_api.h"
#include "bt_app_hf.h"
#include "oled_status.h"
#include "dht11.h"
#include "touch_ctl.h"
#include "bt_app_av.h"
#if CONFIG_EXAMPLE_ENABLE_CONSOLE_REPL
#include "esp_console.h"
#endif
#include "app_hf_msg_set.h"
#include "app_av_msg_set.h"
#include "bt_app_pbac.h"

#define HF_INQUIRY_LEN 30

/* The phone we're talking to (or trying to); see pick_target(). */
esp_bd_addr_t peer_addr = {0};
bool hf_client_connected = false;
static char peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static uint8_t peer_bdname_len;
static const char remote_device_name[] = CONFIG_EXAMPLE_PEER_DEVICE_NAME;

/* Parse "xx:xx:xx:xx:xx:xx" into bda; false (bda untouched) if malformed. */
static bool parse_peer_addr(const char *str, esp_bd_addr_t bda)
{
    unsigned int b[ESP_BD_ADDR_LEN];
    char trailing;
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &trailing) != ESP_BD_ADDR_LEN) {
        return false;
    }
    for (int i = 0; i < ESP_BD_ADDR_LEN; i++) {
        bda[i] = (uint8_t)b[i];
    }
    return true;
}
static bool s_peer_device_found = false;
static char *bda2str(esp_bd_addr_t bda, char *str, size_t size);

/* ---- Reconnect to the phone we know: last used, else any bonded one ----
 * At power-on, and after a link drops, page that phone every
 * RECONNECT_INTERVAL_MS, RECONNECT_TRIES times; then stop and just stay
 * connectable, so a phone the user disconnected on purpose isn't pulled
 * back forever. Any phone can still pair or connect in at any time. */
#define RECONNECT_INTERVAL_MS   15000
#define RECONNECT_TRIES         8
#define NVS_NS                  "bt_app"
#define NVS_KEY_LAST_PEER       "last_peer"

static bool s_have_target;
static bool s_rotate_bonded;    /* no preferred phone: try each paired one in turn */
static int s_bond_idx;
static bool s_link_up;
static int s_tries_left;
static esp_timer_handle_t s_reconnect_timer;

static void save_last_peer(const uint8_t *bda)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_LAST_PEER, bda, ESP_BD_ADDR_LEN);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_last_peer(esp_bd_addr_t bda)
{
    nvs_handle_t h;
    size_t len = ESP_BD_ADDR_LEN;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, NVS_KEY_LAST_PEER, bda, &len) == ESP_OK && len == ESP_BD_ADDR_LEN;
        nvs_close(h);
    }
    return ok;
}

static bool is_bonded(const esp_bd_addr_t bda)
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return false;
    }
    esp_bd_addr_t *list = malloc(n * sizeof(esp_bd_addr_t));
    bool found = false;
    if (list && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
        for (int i = 0; i < n && !found; i++) {
            found = memcmp(list[i], bda, ESP_BD_ADDR_LEN) == 0;
        }
    }
    free(list);
    return found;
}

/* The idx-th paired phone, wrapping around the bond list. */
static bool nth_bonded(int idx, esp_bd_addr_t bda)
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return false;
    }
    esp_bd_addr_t *list = malloc(n * sizeof(esp_bd_addr_t));
    bool ok = list && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK && n > 0;
    if (ok) {
        memcpy(bda, list[idx % n], ESP_BD_ADDR_LEN);
    }
    free(list);
    return ok;
}

/* Configured address, else the last phone that connected (if still
 * paired), else each paired phone in turn. False: nobody paired yet. */
static bool pick_target(esp_bd_addr_t bda, const char **why)
{
    if (parse_peer_addr(CONFIG_EXAMPLE_PEER_DEVICE_ADDR, bda)) {
        *why = "configured";
        return true;
    }
    if (strlen(CONFIG_EXAMPLE_PEER_DEVICE_ADDR) > 0) {
        ESP_LOGW(BT_HF_TAG, "Ignoring malformed peer address '%s' (want xx:xx:xx:xx:xx:xx)",
                 CONFIG_EXAMPLE_PEER_DEVICE_ADDR);
    }
    if (load_last_peer(bda) && is_bonded(bda)) {
        *why = "last used";
        return true;
    }
    if (nth_bonded(0, bda)) {
        *why = "paired";
        s_rotate_bonded = true;
        return true;
    }
    return false;
}

/* Each try arms the timer for the next: a phone that is off or out of range
 * doesn't always produce a disconnect event to retry from. */
static void try_reconnect(void)
{
    if (s_link_up || !s_have_target) {
        return;
    }
    if (s_tries_left <= 0) {
        ESP_LOGI(BT_HF_TAG, "Phone not reachable; waiting for it to connect");
        return;
    }
    s_tries_left--;
    if (s_rotate_bonded) {
        nth_bonded(s_bond_idx++, peer_addr);
    }
    char str[18];
    ESP_LOGI(BT_HF_TAG, "Connecting to %s (%d tries left)...",
             bda2str(peer_addr, str, sizeof(str)), s_tries_left);
    esp_hf_client_connect(peer_addr);
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, RECONNECT_INTERVAL_MS * 1000ULL);
}

static void reconnect_timer_cb(void *arg)
{
    try_reconnect();
}

void bt_app_peer_connected(const uint8_t *bda)
{
    s_link_up = true;
    s_have_target = true;
    s_rotate_bonded = false;    /* from now on this phone is the one to reconnect */
    memcpy(peer_addr, bda, ESP_BD_ADDR_LEN);
    save_last_peer(bda);
    esp_timer_stop(s_reconnect_timer);
}

void bt_app_peer_disconnected(void)
{
    if (!s_link_up) {
        return;     /* a failed attempt; the timer already has the next one */
    }
    /* a live link dropped: start a fresh round of retries */
    s_link_up = false;
    s_tries_left = RECONNECT_TRIES;
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, RECONNECT_INTERVAL_MS * 1000ULL);
}

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, char *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        for (int i = 0; i < param->disc_res.num_prop; i++){
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR
                && get_name_from_eir(param->disc_res.prop[i].val, peer_bdname, &peer_bdname_len)){
                if (strcmp(peer_bdname, remote_device_name) == 0) {
                    s_peer_device_found = true;
                    memcpy(peer_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    ESP_LOGI(BT_HF_TAG, "Found a target device address:");
                    ESP_LOG_BUFFER_HEX(BT_HF_TAG, peer_addr, ESP_BD_ADDR_LEN);
                    ESP_LOGI(BT_HF_TAG, "Found a target device name: %s", peer_bdname);
                    printf("Connect.\n");
                    esp_hf_client_connect(peer_addr);
                    esp_bt_gap_cancel_discovery();
                }
            }
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        ESP_LOGI(BT_HF_TAG, "ESP_BT_GAP_DISC_STATE_CHANGED_EVT");
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && !s_peer_device_found && !hf_client_connected) {
            ESP_LOGI(BT_HF_TAG, "Device discovery failed, continue to discover...");
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, HF_INQUIRY_LEN, 0);
        }
        break;
    case ESP_BT_GAP_RMT_SRVCS_EVT:
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_HF_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(BT_HF_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(BT_HF_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(BT_HF_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(BT_HF_TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(BT_HF_TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_HF_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_HF_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%06"PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_HF_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_HF_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;

    default: {
        ESP_LOGI(BT_HF_TAG, "event: %d", event);
        break;
    }
    }
    return;
}

/* event for handler "bt_av_hdl_stack_up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

#define DHT11_GPIO        GPIO_NUM_4
#define DHT11_PERIOD_MS   5000    /* the sensor itself allows one read per ~2 s */

/* Feeds the display. Pinned to core 1: a read masks interrupts for ~4 ms,
 * which must not land on core 0 where the Bluetooth controller runs. */
static void dht_task(void *arg)
{
    dht11_init(DHT11_GPIO);
    int fails = 0;
    while (1) {
        dht11_data_t d;
        if (dht11_read(DHT11_GPIO, &d) == ESP_OK) {
            g_stat_temp_c = d.temperature;
            g_stat_humidity = d.humidity;
            fails = 0;
        } else if (++fails == 3) {
            ESP_LOGW(BT_HF_TAG, "DHT11: 3 reads in a row failed - check GPIO%d wiring", DHT11_GPIO);
        }
        vTaskDelay(pdMS_TO_TICKS(DHT11_PERIOD_MS));
    }
}

/* handler for bluetooth stack enabled events */
static void bt_hf_client_hdl_stack_evt(uint16_t event, void *p_param);

void app_main(void)
{
    char bda_str[18] = {0};
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    oled_status_start();
    xTaskCreatePinnedToCore(dht_task, "dht11", 3072, NULL, 3, NULL, 1);
    touch_ctl_start();

    ESP_LOGI(BT_HF_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    /* create application task */
    bt_app_task_start_up();

    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_hf_client_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL, NULL);

    /* Voice-over-HCI datapath is used (see sdkconfig.defaults) — audio is fed
     * in through bt_app_hf.c's mic capture task, not fixed PCM GPIO pins or
     * an external AEC chip, so neither of those setup calls is needed here. */

#if CONFIG_EXAMPLE_ENABLE_CONSOLE_REPL
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    repl_config.prompt = "hfp_hf>";

    // init console REPL environment
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    /* Register commands */
    register_hfp_hf();
    register_a2dp_avrc();
    printf("\n ==================================================\n");
    printf(" |       Steps to test hfp_hf                     |\n");
    printf(" |                                                |\n");
    printf(" |  1. Print 'help' to gain overview of commands  |\n");
    printf(" |  2. Setup a service level connection           |\n");
    printf(" |  3. Run hfp_hf to test                         |\n");
    printf(" |                                                |\n");
    printf(" =================================================\n\n");

    // start console REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
#endif
}


static void bt_hf_client_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_HF_TAG, "%s evt %d", __func__, event);
    switch (event) {
    case BT_APP_EVT_STACK_UP: {
        esp_bt_gap_set_device_name(CONFIG_EXAMPLE_LOCAL_DEVICE_NAME);

        /* register GAP callback function */
        esp_bt_gap_register_callback(esp_bt_gap_cb);
        esp_hf_client_register_callback(bt_app_hf_client_cb);
        esp_hf_client_init();
        esp_pbac_register_callback(bt_app_pbac_cb);
        esp_pbac_init();

        /* bring up A2DP sink + AVRCP controller alongside HFP */
        bt_app_av_stack_init();

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

        esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
        esp_bt_pin_code_t pin_code;
        pin_code[0] = '0';
        pin_code[1] = '0';
        pin_code[2] = '0';
        pin_code[3] = '0';
        esp_bt_gap_set_pin(pin_type, 4, pin_code);

        /* set discoverable and connectable mode, wait to be connected */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

        /* Connecting by address is what brings up the HFP service-level
         * connection that makes "Bluetooth" a call audio route; pairing
         * alone isn't enough. */
        const esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .name = "bt_reconnect",
        };
        esp_timer_create(&timer_args, &s_reconnect_timer);
        const char *why = NULL;
        if (pick_target(peer_addr, &why)) {
            char str[18];
            ESP_LOGI(BT_HF_TAG, "Reconnect target: %s phone%s %s", why,
                     s_rotate_bonded ? "s, starting with" : "", bda2str(peer_addr, str, sizeof(str)));
            s_have_target = true;
            s_tries_left = RECONNECT_TRIES;
            try_reconnect();
            /* No discovery alongside: a looping inquiry hogs the radio and
             * starves both this page and the phone connecting in. */
            break;
        }

        if (strlen(CONFIG_EXAMPLE_PEER_DEVICE_NAME) > 0) {
            ESP_LOGI(BT_HF_TAG, "No paired phone; discovering \"%s\"...", CONFIG_EXAMPLE_PEER_DEVICE_NAME);
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, HF_INQUIRY_LEN, 0);
        } else {
            ESP_LOGI(BT_HF_TAG, "No paired phone yet; pair with \"%s\" from the phone",
                     CONFIG_EXAMPLE_LOCAL_DEVICE_NAME);
        }
        break;
    }
    default:
        ESP_LOGE(BT_HF_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}
