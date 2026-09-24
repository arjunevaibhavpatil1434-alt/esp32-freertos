/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
*
* Classic Bluetooth SPP acceptor with pairing support. The ESP32 advertises
* itself as connectable/discoverable, accepts an incoming SPP connection
* (e.g. from a phone using a "Serial Bluetooth Terminal" style app), handles
* both legacy PIN pairing and Secure Simple Pairing, and echoes back any data
* it receives so the link can be verified end-to-end.
*
****************************************************************************/

#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#define TAG               "BT_SPP"
#define LOCAL_DEVICE_NAME "ESP32_SPP_ACCEPTOR"
#define SPP_SERVER_NAME   "SPP_SERVER"

static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

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

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "SPP initialized, starting server '%s'", SPP_SERVER_NAME);
            esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
        } else {
            ESP_LOGE(TAG, "SPP init failed, status:%d", param->init.status);
        }
        break;

    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "SPP server started, scn:%d", param->start.scn);
            esp_bt_gap_set_device_name(LOCAL_DEVICE_NAME);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            ESP_LOGI(TAG, "Discoverable as '%s' — pair to it from your phone", LOCAL_DEVICE_NAME);
        } else {
            ESP_LOGE(TAG, "SPP server start failed, status:%d", param->start.status);
        }
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(TAG, "Client connected, handle:%" PRIu32 " addr:[%s]",
                 param->srv_open.handle,
                 bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        break;

    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "Connection closed, handle:%" PRIu32, param->close.handle);
        break;

    case ESP_SPP_DATA_IND_EVT:
        ESP_LOGI(TAG, "Received %d bytes on handle:%" PRIu32,
                 param->data_ind.len, param->data_ind.handle);
        if (param->data_ind.len > 0) {
            ESP_LOG_BUFFER_HEXDUMP(TAG, param->data_ind.data, param->data_ind.len, ESP_LOG_INFO);
            /* Echo back so the phone side can confirm two-way data flow. */
            esp_spp_write(param->data_ind.handle, param->data_ind.len, param->data_ind.data);
        }
        break;

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
        /* Legacy pairing (pre-2.1 devices, or when SSP is unsupported). */
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
        /* Secure Simple Pairing "Numeric Comparison" — auto-confirm. */
        ESP_LOGI(TAG, "SSP numeric comparison, value:%06" PRIu32 " — auto-confirming",
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey (enter this on the phone if asked): %06" PRIu32,
                 param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey requested from peer");
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "Link mode changed:%d [%s]", param->mode_chg.mode,
                 bda2str(param->mode_chg.bda, bda_str, sizeof(bda_str)));
        break;

    default:
        ESP_LOGI(TAG, "GAP event: %d", event);
        break;
    }
}

void app_main(void)
{
    char bda_str[18] = {0};

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "gap callback register failed: %s", esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_spp_register_callback(esp_spp_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "spp callback register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0, /* only used for ESP_SPP_MODE_VFS */
    };
    if ((ret = esp_spp_enhanced_init(&bt_spp_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "spp init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Secure Simple Pairing: "Display Yes/No" — we auto-confirm in the GAP callback. */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));

    /* Legacy pairing fallback: variable PIN, supplied in the GAP callback. */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
}
