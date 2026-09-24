#include "ui.h"

static lv_obj_t *label_title;
static lv_obj_t *label_reading;
static lv_obj_t *label_status;

void sensor_ui_init(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);

    label_title = lv_label_create(scr);
    lv_label_set_text(label_title, "ESP32 Sensor Hub");
    lv_obj_set_width(label_title, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 0);

    label_reading = lv_label_create(scr);
    lv_label_set_text(label_reading, "Reading...");
    lv_obj_align(label_reading, LV_ALIGN_CENTER, 0, 0);

    label_status = lv_label_create(scr);
    lv_label_set_text(label_status, "BT: waiting");
    lv_obj_set_width(label_status, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void sensor_ui_update(int temp_c, int humidity, bool valid, bool bt_connected)
{
    if (valid) {
        lv_label_set_text_fmt(label_reading, "%dC   %d%%RH", temp_c, humidity);
    } else {
        lv_label_set_text(label_reading, "sensor error");
    }
    lv_label_set_text(label_status, bt_connected ? "BT: connected" : "BT: waiting");
}

void sensor_ui_set_bt_status(bool bt_connected)
{
    lv_label_set_text(label_status, bt_connected ? "BT: connected" : "BT: waiting");
}
