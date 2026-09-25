#ifndef __TOUCH_CTL_H__
#define __TOUCH_CTL_H__

/* One capacitive touch pad on GPIO13 (touch channel T4), using the ESP32's
 * built-in touch sensor; the pad is just a wire end, foil or coin.
 *
 *   phone ringing:      tap = answer, long press = reject
 *   dialing / in call:  tap or long press = hang up
 *   otherwise:          tap = music play / pause
 *
 * Calibrates at start-up, so don't touch the pad for the first ~2 s after
 * power-on or reset. */
void touch_ctl_start(void);

#endif /* __TOUCH_CTL_H__ */
