#ifndef __SPK_I2S_H__
#define __SPK_I2S_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/* MAX98357A I2S amp on I2S0 (BCLK 27, LRC 14, DIN 25), shared by call audio
 * (bt_app_hf.c, 16 kHz) and music (bt_app_av.c, usually 44.1 kHz). It takes
 * 16-bit mono samples and sends each on both slots. */

/* Brings the amp up on first use, and switches its sample rate when it
 * differs from the current one. */
bool spk_i2s_set_rate(uint32_t rate);

/* Blocks until the samples are queued, or `timeout` passes. */
void spk_i2s_write(const int16_t *pcm, size_t samples, TickType_t timeout);

#endif /* __SPK_I2S_H__ */
