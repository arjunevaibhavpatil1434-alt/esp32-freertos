#ifndef __OLED_STATUS_H__
#define __OLED_STATUS_H__

#include <stdbool.h>
#include <stdint.h>

/* Written by bt_app_hf.c, bt_app_av.c and the DHT11 task in main.c, drawn on the SSD1306 by its own task. */
extern volatile bool g_stat_slc;        /* HFP service-level link up */
extern volatile bool g_stat_in_call;
extern volatile bool g_stat_ringing;
extern volatile bool g_stat_outgoing;   /* dialing or alerting */
extern volatile bool g_stat_audio;      /* SCO/eSCO audio link up */
extern volatile bool g_stat_wideband;   /* mSBC rather than CVSD */
extern volatile uint32_t g_stat_music_tick;  /* tick of the last A2DP audio packet */
extern volatile int g_stat_temp_c;      /* DHT11 reading, -1 until the first one */
extern volatile int g_stat_humidity;

/* Starts the redraw task, which brings up the OLED on I2C (SDA21/SCL22,
 * 0x3C) and keeps retrying if it doesn't answer; Bluetooth carries on
 * either way. */
void oled_status_start(void);

/* Caller shown during a call: name (may be NULL/empty) and number. Both
 * NULL clears it. Non-ASCII characters are drawn as '?'. */
void oled_status_set_caller(const char *name, const char *number);

/* Track shown while music plays; NULL clears that field. */
void oled_status_set_track_title(const char *title);
void oled_status_set_track_artist(const char *artist);

#endif /* __OLED_STATUS_H__ */
