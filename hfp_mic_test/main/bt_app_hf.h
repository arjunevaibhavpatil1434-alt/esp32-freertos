/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef __BT_APP_HF_H__
#define __BT_APP_HF_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_hf_client_api.h"


#define BT_HF_TAG               "BT_HF"

/**
 * @brief     callback function for HF client
 */
void bt_app_hf_client_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param);

/* From main.c: remember the phone and stop / start reconnect attempts. */
void bt_app_peer_connected(const uint8_t *bda);
void bt_app_peer_disconnected(void);

/* True while call audio owns the speaker; music stays quiet meanwhile. */
bool bt_app_hf_audio_active(void);


#endif /* __BT_APP_HF_H__*/
