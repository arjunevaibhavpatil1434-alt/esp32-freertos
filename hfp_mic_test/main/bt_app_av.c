/*
 * A2DP sink (music streaming) + AVRCP controller (playback control/metadata),
 * running alongside the HFP hands-free client in bt_app_hf.c. There's no
 * speaker wired up, so incoming A2DP audio is just byte-counted and
 * discarded — this exists to let the phone treat the ESP32 as a full
 * Bluetooth headset (calls via HFP, music transport + control via A2DP/AVRCP)
 * and to exercise playback control (play/pause/next/prev) and track
 * metadata over the console.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_legacy_api.h"

#include "bt_app_core.h"
#include "bt_app_av.h"

static const char *s_a2d_conn_state_str[] = {"Disconnected", "Connecting", "Connected", "Disconnecting"};
static const char *s_a2d_audio_state_str[] = {"Suspended", "Started"};

static uint32_t s_a2d_pkt_cnt = 0;
static uint8_t s_avrc_tl = 0;

/* ---------------- A2DP sink ---------------- */

static void bt_app_a2d_evt_hdl(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)param;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        uint8_t *bda = a2d->conn_stat.remote_bda;
        ESP_LOGI(BT_AV_TAG, "A2DP connection state: %s, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 s_a2d_conn_state_str[a2d->conn_stat.state], bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "A2DP audio state: %s", s_a2d_audio_state_str[a2d->audio_stat.state]);
        if (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_a2d_pkt_cnt = 0;
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(BT_AV_TAG, "A2DP audio stream configured, codec type: %d", a2d->audio_cfg.mcc.type);
        break;
    case ESP_A2D_PROF_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "A2DP PROF STATE: %s",
                 a2d->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS ? "Init Complete" : "Deinit Complete");
        break;
    default:
        ESP_LOGD(BT_AV_TAG, "unhandled A2DP event: %d", event);
        break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_app_work_dispatch(bt_app_a2d_evt_hdl, event, param, sizeof(esp_a2d_cb_param_t), NULL, NULL);
}

/* No speaker wired — just log stream activity every ~100 packets. */
static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (++s_a2d_pkt_cnt % 100 == 0) {
        ESP_LOGI(BT_AV_TAG, "A2DP audio packets received: %" PRIu32 " (discarded — no speaker wired)", s_a2d_pkt_cnt);
    }
}

/* ---------------- AVRCP controller ---------------- */

static void bt_app_av_copy_metadata(bt_app_msg_t *msg, void *p_dest, void *p_src)
{
    if (msg->event != ESP_AVRC_CT_METADATA_RSP_EVT) {
        return;
    }
    esp_avrc_ct_cb_param_t *dest = (esp_avrc_ct_cb_param_t *)p_dest;
    esp_avrc_ct_cb_param_t *src = (esp_avrc_ct_cb_param_t *)p_src;

    dest->meta_rsp.attr_text = (uint8_t *)malloc(src->meta_rsp.attr_length + 1);
    if (dest->meta_rsp.attr_text) {
        memcpy(dest->meta_rsp.attr_text, src->meta_rsp.attr_text, src->meta_rsp.attr_length);
        dest->meta_rsp.attr_text[src->meta_rsp.attr_length] = '\0';
    }
}

static void bt_app_av_free_metadata(void *p_param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)p_param;
    if (rc && rc->meta_rsp.attr_text) {
        free(rc->meta_rsp.attr_text);
        rc->meta_rsp.attr_text = NULL;
    }
}

static const char *avrc_attr_name(uint8_t attr_id)
{
    switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:  return "Title";
    case ESP_AVRC_MD_ATTR_ARTIST: return "Artist";
    case ESP_AVRC_MD_ATTR_ALBUM:  return "Album";
    default: return "Attr";
    }
}

static void bt_app_avrc_ct_evt_hdl(uint16_t event, void *param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)param;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP %s", rc->conn_stat.connected ? "connected" : "disconnected");
        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_metadata_cmd(s_avrc_tl++,
                ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
        }
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP %s: %s", avrc_attr_name(rc->meta_rsp.attr_id),
                 rc->meta_rsp.attr_text ? (char *)rc->meta_rsp.attr_text : "(none)");
        break;
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP passthrough rsp: key_code 0x%x, key_state %d, rsp %d",
                 rc->psth_rsp.key_code, rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;
    case ESP_AVRC_CT_PROF_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP CT PROF STATE: %s",
                 rc->avrc_ct_init_stat.state == ESP_AVRC_INIT_SUCCESS ? "Init Complete" : "Deinit Complete");
        break;
    default:
        ESP_LOGD(BT_AV_TAG, "unhandled AVRCP event: %d", event);
        break;
    }
}

static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_METADATA_RSP_EVT) {
        bt_app_work_dispatch(bt_app_avrc_ct_evt_hdl, event, param, sizeof(esp_avrc_ct_cb_param_t),
                              bt_app_av_copy_metadata, bt_app_av_free_metadata);
    } else {
        bt_app_work_dispatch(bt_app_avrc_ct_evt_hdl, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL, NULL);
    }
}

/* ---------------- Playback control (used by console commands) ---------------- */

void bt_app_av_passthrough(uint8_t key_code)
{
    esp_avrc_ct_send_passthrough_cmd(s_avrc_tl++, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(s_avrc_tl++, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bt_app_av_request_metadata(void)
{
    esp_avrc_ct_send_metadata_cmd(s_avrc_tl++,
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
}

/* ---------------- Init ---------------- */

void bt_app_av_stack_init(void)
{
    esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
    ESP_ERROR_CHECK(esp_avrc_ct_init());

    esp_a2d_register_callback(bt_app_a2d_cb);
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);
}
