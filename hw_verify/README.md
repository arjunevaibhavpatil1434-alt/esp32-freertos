# Hardware Verification

Flashable test firmware that exercises the DHT11, OLED, and I2S mic wired to
the ESP32 on the breadboard, and prints pass/fail results over serial. Used
to confirm breadboard wiring without needing a camera on the setup.

## Verified pinout

| Component | Signal | ESP32 GPIO | Notes |
|---|---|---|---|
| DHT11 | DATA | GPIO4 | Single-wire, internal pull-up enabled by driver |
| OLED (SSD1306/SH1106) | SDA | GPIO21 | I2C, detected at address `0x3C` |
| OLED (SSD1306/SH1106) | SCL | GPIO22 | |
| INMP441 mic | WS (word-select) | GPIO33 | Was GPIO25; moved so GPIO25 (DAC) can drive the speaker in `hfp_mic_test` |
| INMP441 mic | SCK (bit-clock) | GPIO26 | |
| INMP441 mic | SD (data out) | GPIO32 | |
| INMP441 mic | L/R | GND | Selects left channel; firmware reads `I2S_STD_SLOT_LEFT` |

All components share VCC (3.3V) and GND with the ESP32.

### Mic pin history

The mic was originally wired to WS=GPIO5, SCK=GPIO19, SD=GPIO18. That
location on the breadboard produced consistent garbage/flat-zero readings
across multiple rewiring attempts (bad row or bad jumper), even after
swapping WS/SCK and reseating SD. Moving to WS=GPIO25, SCK=GPIO26, SD=GPIO32
fixed it immediately — real signal with a power-on transient decaying to a
steady noise floor. Avoid reusing GPIO5/18/19 breadboard rows for this mic.

Later WS moved again, from GPIO25 to GPIO33, because GPIO25 is one of only
two DAC pins and `hfp_mic_test` uses it for speaker output.

## Speaker (hfp_mic_test only)

| Component | Signal | ESP32 GPIO | Notes |
|---|---|---|---|
| Powered speaker, 3.5mm AUX in | TIP + RING | GPIO25 (DAC ch 0) | Through a 1–10µF DC-blocking cap (+ toward ESP32) and ~1kΩ series resistor |
| Powered speaker, 3.5mm AUX in | SLEEVE | GND | |

## Running the test

```bash
source /home/server/esp32-freertos/esp-idf/export.sh
cd /home/server/esp32-freertos/hw_verify
idf.py -p /dev/ttyUSB0 flash monitor   # Ctrl+] to exit monitor
```

Expected output on a good build:

- `[DHT11] RESULT: 5 ok / 0 failed` — temperature/humidity read successfully
- `[OLED] Found device at 0x3C` — I2C bus scan found the display
- `[MIC] RESULT: signal detected` — I2S mic peak amplitude is non-trivial and
  varies over time (not stuck at 0 or pegged at max)
