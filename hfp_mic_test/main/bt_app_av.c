/*
 * A2DP sink (music streaming) + AVRCP controller (playback control/metadata),
 * running alongside the HFP hands-free client in bt_app_hf.c. Music plays on
 * the MAX98357A (spk_i2s.c), mixed down to mono since the amp's SD pin
 * selects one channel. Calls take the speaker over while they have audio.
 * Playback control (play/pause/next/prev) and track metadata are on the
 * console.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_legacy_api.h"

#include "bt_app_core.h"
#include "bt_app_av.h"
#include "bt_app_hf.h"
#include "oled_status.h"
#include "spk_i2s.h"

static const char *s_a2d_conn_state_str[] = {"Disconnected", "Connecting", "Connected", "Disconnecting"};
static const char *s_a2d_audio_state_str[] = {"Suspended", "Started"};

static uint32_t s_a2d_pkt_cnt = 0;
static volatile uint32_t s_a2d_rate = 44100;   /* from the stream config */
static volatile int s_a2d_channels = 2;
static uint8_t s_avrc_tl = 0;
static esp_avrc_rn_evt_cap_mask_t s_peer_rn_cap;   /* notifications the phone supports */

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
            /* in case the phone skips the track-change notification */
            bt_app_av_request_metadata();
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_cie_sbc_t *sbc = &a2d->audio_cfg.mcc.cie.sbc_info;
        if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            s_a2d_rate = (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_48K) ? 48000
                       : (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_32K) ? 32000
                       : (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_16K) ? 16000 : 44100;
            s_a2d_channels = (sbc->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
        }
        ESP_LOGI(BT_AV_TAG, "A2DP audio stream configured, codec type: %d, %lu Hz, %d ch",
                 a2d->audio_cfg.mcc.type, (unsigned long)s_a2d_rate, s_a2d_channels);
        break;
    }
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

/* Decoded 16-bit PCM from the SBC decoder, in the BT stack's context. The
 * I2S write blocks until DMA has room, which paces the stream; its timeout
 * keeps a stalled amp from holding the stack up. */
static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    g_stat_music_tick = xTaskGetTickCount();
    if (++s_a2d_pkt_cnt % 500 == 0) {
        ESP_LOGI(BT_AV_TAG, "A2DP audio packets played: %" PRIu32, s_a2d_pkt_cnt);
    }
    if (bt_app_hf_audio_active() || !spk_i2s_set_rate(s_a2d_rate)) {
        return;
    }

    const int16_t *pcm = (const int16_t *)data;
    int ch = s_a2d_channels;
    uint32_t frames = len / (sizeof(int16_t) * ch);
    int16_t mono[256];
    uint32_t n = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = ch == 2 ? (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2) : pcm[i];
        mono[n++] = v;
        if (n == sizeof(mono) / sizeof(mono[0]) || i == frames - 1) {
            spk_i2s_write(mono, n, pdMS_TO_TICKS(100));
            n = 0;
        }
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

/* Phones send a placeholder like "unknow" when nothing is loaded. */
static const char *track_text(const uint8_t *text)
{
    const char *t = (const char *)text;
    if (!t || !t[0] || !strcasecmp(t, "unknow") || !strcasecmp(t, "unknown")) {
        return NULL;
    }
    return t;
}

static void bt_app_avrc_request_track(void)
{
    esp_avrc_ct_send_metadata_cmd(s_avrc_tl++, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
}

/* Notifications are one-shot: re-register after each one to get the next. */
static void bt_app_avrc_register_track_change(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_peer_rn_cap,
                                           ESP_AVRC_RN_TRACK_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(s_avrc_tl++, ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
}

static void bt_app_avrc_ct_evt_hdl(uint16_t event, void *param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)param;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP %s", rc->conn_stat.connected ? "connected" : "disconnected");
        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(s_avrc_tl++);
            bt_app_avrc_request_track();
        } else {
            s_peer_rn_cap.bits = 0;
            oled_status_set_track_title(NULL);
            oled_status_set_track_artist(NULL);
        }
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        ESP_LOGI(BT_AV_TAG, "AVRCP phone notification caps 0x%x", s_peer_rn_cap.bits);
        bt_app_avrc_register_track_change();
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (rc->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            bt_app_avrc_request_track();
            bt_app_avrc_register_track_change();
        }
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP %s: %s", avrc_attr_name(rc->meta_rsp.attr_id),
                 rc->meta_rsp.attr_text ? (char *)rc->meta_rsp.attr_text : "(none)");
        if (rc->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE) {
            oled_status_set_track_title(track_text(rc->meta_rsp.attr_text));
        } else if (rc->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ARTIST) {
            oled_status_set_track_artist(track_text(rc->meta_rsp.attr_text));
        }
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
