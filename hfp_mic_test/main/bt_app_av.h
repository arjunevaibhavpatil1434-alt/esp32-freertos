#ifndef __BT_APP_AV_H__
#define __BT_APP_AV_H__

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#define BT_AV_TAG    "BT_AV"

/* Bring up A2DP sink + AVRCP controller. Called once from the BT stack-up
 * handler, alongside the existing HFP init. */
void bt_app_av_stack_init(void);

/* Sends an AVRCP passthrough key press+release, e.g. ESP_AVRC_PT_CMD_PLAY. */
void bt_app_av_passthrough(uint8_t key_code);

/* Requests title/artist/album metadata for the current track. */
void bt_app_av_request_metadata(void);

#endif /* __BT_APP_AV_H__ */
