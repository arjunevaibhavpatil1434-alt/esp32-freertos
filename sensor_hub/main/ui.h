#ifndef SENSOR_UI_H
#define SENSOR_UI_H

#include <stdbool.h>
#include "lvgl.h"

/* Creates the three-line layout (title / reading / BT status) on the given
 * LVGL display. Caller must hold the LVGL lock. */
void sensor_ui_init(lv_display_t *disp);

/* Updates the reading and BT status lines. Caller must hold the LVGL lock. */
void sensor_ui_update(int temp_c, int humidity, bool valid, bool bt_connected);

/* Updates only the BT status line, leaving the reading untouched. Caller must hold the LVGL lock. */
void sensor_ui_set_bt_status(bool bt_connected);

#endif /* SENSOR_UI_H */
