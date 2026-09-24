/*
 * Console commands for A2DP sink / AVRCP controller, mirroring the style of
 * app_hf_msg_set.c.
 */

#include <stdio.h>
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_console.h"

#include "bt_app_av.h"
#include "app_av_msg_set.h"

extern esp_bd_addr_t peer_addr;

#define AV_CMD_HANDLER(cmd)    static int av_##cmd##_handler(int argn, char **argv)

AV_CMD_HANDLER(conn)
{
    printf("a2dp connect\n");
    esp_a2d_sink_connect(peer_addr);
    return 0;
}

AV_CMD_HANDLER(disc)
{
    printf("a2dp disconnect\n");
    esp_a2d_sink_disconnect(peer_addr);
    return 0;
}

AV_CMD_HANDLER(play)
{
    printf("avrcp play\n");
    bt_app_av_passthrough(ESP_AVRC_PT_CMD_PLAY);
    return 0;
}

AV_CMD_HANDLER(pause)
{
    printf("avrcp pause\n");
    bt_app_av_passthrough(ESP_AVRC_PT_CMD_PAUSE);
    return 0;
}

AV_CMD_HANDLER(next)
{
    printf("avrcp next track\n");
    bt_app_av_passthrough(ESP_AVRC_PT_CMD_FORWARD);
    return 0;
}

AV_CMD_HANDLER(prev)
{
    printf("avrcp previous track\n");
    bt_app_av_passthrough(ESP_AVRC_PT_CMD_BACKWARD);
    return 0;
}

AV_CMD_HANDLER(metadata)
{
    printf("avrcp request metadata\n");
    bt_app_av_request_metadata();
    return 0;
}

void register_a2dp_avrc(void)
{
    const esp_console_cmd_t acon_cmd = {
        .command = "acon",
        .help = "connect A2DP sink to peer device",
        .hint = NULL,
        .func = av_conn_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&acon_cmd));

    const esp_console_cmd_t adis_cmd = {
        .command = "adis",
        .help = "disconnect A2DP sink from peer device",
        .hint = NULL,
        .func = av_disc_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&adis_cmd));

    const esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "AVRCP: play",
        .hint = NULL,
        .func = av_play_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&play_cmd));

    const esp_console_cmd_t pause_cmd = {
        .command = "pause",
        .help = "AVRCP: pause",
        .hint = NULL,
        .func = av_pause_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pause_cmd));

    const esp_console_cmd_t next_cmd = {
        .command = "next",
        .help = "AVRCP: next track",
        .hint = NULL,
        .func = av_next_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&next_cmd));

    const esp_console_cmd_t prev_cmd = {
        .command = "prev",
        .help = "AVRCP: previous track",
        .hint = NULL,
        .func = av_prev_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&prev_cmd));

    const esp_console_cmd_t md_cmd = {
        .command = "md",
        .help = "AVRCP: request title/artist/album metadata",
        .hint = NULL,
        .func = av_metadata_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&md_cmd));
}
