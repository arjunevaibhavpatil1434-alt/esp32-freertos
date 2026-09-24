# esp32-freertos

ESP32 + FreeRTOS test projects, built against ESP-IDF.

## Projects

| Directory | What it does |
|---|---|
| [`freertos_test`](freertos_test/) | Minimal FreeRTOS hello-world |
| [`bluetooth_test`](bluetooth_test/) | Classic Bluetooth SPP server (`ESP32_SPP_ACCEPTOR`) with PIN/SSP pairing that echoes back whatever a phone serial terminal sends |
| [`dht11_temperature`](dht11_temperature/) | Reads temperature/humidity from a DHT11 sensor |
| [`hw_verify`](hw_verify/) | Flashable firmware that checks DHT11, OLED (I2C), and an I2S mic are wired correctly on the breadboard, printing pass/fail results over serial |
| [`sensor_hub`](sensor_hub/) | DHT11 → OLED (LVGL) → Bluetooth SPP pipeline: shows readings on the OLED and pushes them to a paired phone every 5s (send `GET` for an immediate reading) |
| [`hfp_mic_test`](hfp_mic_test/) | Bluetooth Hands-Free client that auto-connects to a known phone and feeds the I2S mic into calls, plus A2DP sink / AVRCP controls |

For the full walkthrough (wiring, setup from scratch, every file, the
Bluetooth profiles and audio pipelines, commands, test results and
troubleshooting), see [docs/PROJECT_GUIDE.md](docs/PROJECT_GUIDE.md).

## Setup

This repo does not include the ESP-IDF SDK itself (it's excluded via
`.gitignore` since it's a large, separately-versioned checkout). Clone it
alongside this repo:

```bash
git clone --recursive https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32
```

## Building and flashing a project

```bash
source /path/to/esp-idf/export.sh
cd <project-directory>
idf.py set-target esp32
idf.py -p /dev/ttyUSB0 flash monitor   # Ctrl+] to exit monitor
```

See each project's own README for specifics (wiring, expected output, etc).
