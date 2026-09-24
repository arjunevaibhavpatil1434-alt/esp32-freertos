# ESP32 FreeRTOS Projects: Complete Build & Pipeline Guide

Last updated: 2026-09-24

## 1. Overview

This repo holds six ESP-IDF firmware projects for one ESP32 on a breadboard. Together they build up to two working pipelines: a sensor hub (DHT11 to OLED to Bluetooth) and a Bluetooth hands-free unit that sends an I2S mic into phone calls.

- **Repository:** github.com/arjunevaibhavpatil1434-alt/esp32-freertos, branch `master`
- **SDK:** ESP-IDF v6.1-dev (FreeRTOS kernel, Bluedroid Bluetooth stack), target `esp32`
- **Board:** ESP32 dev board on `/dev/ttyUSB0`, chip revision v3.1, 4 MB flash (image header says 2 MB)

| Project | Role | Status (2026-09-24) |
| --- | --- | --- |
| `freertos_test` | Minimal FreeRTOS hello-world | Builds |
| `dht11_temperature` | Read DHT11 temperature/humidity | Builds |
| `hw_verify` | Breadboard wiring self-test: DHT11, OLED, mic | Passed on hardware |
| `bluetooth_test` | Classic BT SPP echo server | pytest passed on hardware |
| `sensor_hub` | DHT11 → OLED → Bluetooth SPP pipeline | Passed on hardware (after DHT11 fix) |
| `hfp_mic_test` | Hands-free client: mic into calls, call audio to speaker | Mic verified on a real call; speaker untested |

```mermaid
flowchart LR
    DHT[DHT11<br/>GPIO4] --> ESP[ESP32<br/>FreeRTOS + Bluedroid]
    MIC[INMP441 mic<br/>I2S1: GPIO33/26/32] --> ESP
    ESP --> OLED[SSD1306 OLED<br/>I2C 0x3C]
    ESP --> DAC[GPIO25 DAC<br/>to amp/speaker]
    ESP <-->|SPP: readings + GET| PH1[Phone<br/>serial terminal]
    ESP <-->|HFP + A2DP + AVRCP + PBAP| PH2[POCO F4<br/>calls and music]
```

Sensors and the mic feed into the ESP32. Readings go out to the OLED and over SPP; call audio goes both ways over HFP.

## 2. Hardware and wiring

Every part shares the ESP32's 3.3 V and GND. The pin map below is the one currently wired and used by the firmware; the mic WS moved from GPIO25 to GPIO33 on 2026-09-24 so GPIO25 could become the speaker output.

### Bill of materials

| Part | Used by | Notes |
| --- | --- | --- |
| ESP32 dev board (ESP32-D0WD, rev v3.1) | All | USB-serial on `/dev/ttyUSB0` |
| DHT11 temperature/humidity sensor | dht11\_temperature, hw\_verify, sensor\_hub | Single-wire, internal pull-up |
| SSD1306/SH1106 128×64 OLED, I2C | hw\_verify, sensor\_hub | Address `0x3C` |
| INMP441 I2S MEMS mic | hw\_verify, hfp\_mic\_test | 24-bit, left channel |
| Speaker path (pick one) | hfp\_mic\_test | Powered AUX speaker, or PAM8403 amp + 4 Ω speaker |
| 1–10 µF capacitor + 1 kΩ resistor | Speaker path | DC block and level drop for AUX / amp input |
| Android phone (POCO F4) | bluetooth\_test, sensor\_hub, hfp\_mic\_test | Host PC has no Bluetooth adapter |

### Pin map

| Component | Signal | ESP32 GPIO | Notes |
| --- | --- | --- | --- |
| DHT11 | DATA | GPIO4 | Bit-banged, timing-critical |
| OLED | SDA | GPIO21 | I2C port 0, 400 kHz |
| OLED | SCL | GPIO22 |  |
| INMP441 | WS (word select) | GPIO33 | Was GPIO25 until 2026-09-24 |
| INMP441 | SCK (bit clock) | GPIO26 |  |
| INMP441 | SD (data) | GPIO32 |  |
| INMP441 | L/R | GND | Selects left slot |
| Speaker | Audio out | GPIO25 | DAC channel 0, 8-bit |

GPIO5, 18 and 19 are avoided for the mic: those breadboard rows gave garbage readings in the past.

### Speaker wiring options

1. **Powered speaker with AUX in** (no purchase): GPIO25 → 1–10 µF cap (+ toward ESP32) → 1 kΩ → plug TIP and RING; GND → SLEEVE.
2. **PAM8403 amp + 4 Ω 5 W speaker** (planned): GPIO25 → 1 µF cap → L-in; GND → GND; ESP32 5V/VIN → 5V; speaker on L+ / L−.
3. **Bare 4 Ω speaker, no amp** (check only, very quiet): GPIO25 → 100 µF cap (+ toward ESP32) → 100 Ω → speaker +; GND → speaker −. Never connect it without the capacitor.

```text
GPIO25 ──[+ cap −]──[resistor]──┬── TIP  (left)
                                └── RING (right)
GND ─────────────────────────────── SLEEVE (ground)
```

## 3. Setting up from scratch

A fresh Ubuntu machine needs system packages, the ESP-IDF checkout inside the repo, and serial-port access. The versions below are the ones this repo was last built with.

| Tool | Version in use |
| --- | --- |
| Host OS | Ubuntu 22.04.5 LTS |
| ESP-IDF | v6.1-dev-7947-g8438b4b0ca0 (commit 8438b4b0ca0, 2026-09-16) |
| Compiler | xtensa-esp-elf-gcc 16.1.0 (esp-16.1.0\_20260609) |
| CMake / Ninja | 3.22.1 / 1.10.1 |
| Python | 3.10.12, venv `~/.espressif/python_env/idf6.2_py3.10_env` |
| esptool | 5.4.0 |
| pytest / pytest-embedded | 9.1.1 / 2.9.3 |

### Steps

1. Install system packages:

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

2. Clone the project, then ESP-IDF inside it. `esp-idf/` is in `.gitignore`, so it never gets committed:

```bash
git clone https://github.com/arjunevaibhavpatil1434-alt/esp32-freertos.git
cd esp32-freertos
git clone --recursive https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32
```

3. Let your user open the serial port, then log out and back in:

```bash
sudo usermod -aG dialout $USER
ls -l /dev/ttyUSB0      # crw-rw---- root dialout
```

4. Load the environment in every new shell:

```bash
source esp-idf/export.sh
idf.py --version
```

5. Optional, for automated hardware tests (section 10):

```bash
pip install pytest-embedded pytest-embedded-idf pytest-embedded-serial-esp
```

6. Build and flash a first project to prove the chain works:

```bash
cd freertos_test
idf.py set-target esp32
idf.py -p /dev/ttyUSB0 flash monitor   # Ctrl+] exits the monitor
```

## 4. Repository layout

Git tracks 62 files across six projects; about 3,550 lines of C live in the `main/` folders. Each project is a standalone ESP-IDF app with the same shape: a top-level `CMakeLists.txt`, a `main/` component, and a committed `sdkconfig`.

### Tree

```text
esp32-freertos/
├── .gitignore                     # ignores esp-idf/, build/, managed_components/, dependencies.lock, sdkconfig.old
├── README.md                      # project index + setup
├── esp-idf/                       # ESP-IDF SDK checkout (NOT tracked)
│
├── freertos_test/                 # 1. FreeRTOS hello-world
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── pytest_hello_world.py
│   ├── sdkconfig  ·  sdkconfig.ci
│   └── main/
│       ├── CMakeLists.txt
│       └── hello_world_main.c
│
├── dht11_temperature/             # 2. DHT11 reader
│   ├── CMakeLists.txt  ·  README.md  ·  pytest_hello_world.py
│   ├── sdkconfig  ·  sdkconfig.ci
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.c                 # 2 s read loop
│       └── dht11.c  ·  dht11.h     # bit-bang driver
│
├── hw_verify/                     # 3. wiring self-test
│   ├── CMakeLists.txt  ·  README.md (pin map)  ·  sdkconfig
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.c                 # DHT11 x5, I2C scan, mic peak level
│       └── dht11.c  ·  dht11.h
│
├── bluetooth_test/                # 4. SPP echo server
│   ├── CMakeLists.txt             # project(bluetooth_test)
│   ├── README.md
│   ├── pytest_bt_spp_test.py      # hardware test
│   ├── sdkconfig  ·  sdkconfig.defaults  ·  sdkconfig.defaults.esp32s31
│   └── main/
│       ├── CMakeLists.txt
│       └── main.c                 # GAP + SPP callbacks
│
├── sensor_hub/                    # 5. DHT11 -> OLED -> SPP pipeline
│   ├── CMakeLists.txt  ·  sdkconfig  ·  sdkconfig.defaults
│   └── main/
│       ├── CMakeLists.txt
│       ├── idf_component.yml      # lvgl/lvgl 9.2.0
│       ├── main.c                 # OLED, BT, sensor task
│       ├── dht11.c  ·  dht11.h     # critical-section version
│       └── ui.c  ·  ui.h           # LVGL screen
│
└── hfp_mic_test/                  # 6. hands-free: mic + speaker
    ├── CMakeLists.txt  ·  sdkconfig  ·  sdkconfig.defaults
    └── main/
        ├── CMakeLists.txt
        ├── Kconfig.projbuild      # peer name/address, SSP, console
        ├── main.c                 # BT init, GAP, discovery, auto-connect
        ├── bt_app_core.c/.h       # app task + message queue
        ├── bt_app_hf.c/.h         # HFP client, mic uplink, DAC speaker
        ├── bt_app_av.c/.h         # A2DP sink + AVRCP controller
        ├── bt_app_pbac.c/.h       # PBAP phonebook client
        ├── app_hf_msg_set.c/.h    # console commands for HFP
        └── app_av_msg_set.c/.h    # console commands for A2DP/AVRCP
```

### What each file type does

| File | Purpose | Tracked |
| --- | --- | --- |
| `CMakeLists.txt` (project root) | Includes `project.cmake`, names the app | Yes |
| `main/CMakeLists.txt` | `idf_component_register`: sources + `PRIV_REQUIRES` components | Yes |
| `sdkconfig` | Full generated config for the app | Yes |
| `sdkconfig.defaults` | Hand-written overrides applied when `sdkconfig` is created | Yes |
| `main/Kconfig.projbuild` | Adds project options to `menuconfig` | Yes |
| `main/idf_component.yml` | Component-manager dependencies | Yes |
| `pytest_*.py` | pytest-embedded hardware tests | Yes |
| `build/` | Compiler output, `.bin`/`.elf`, logs | No |
| `managed_components/`, `dependencies.lock` | Downloaded registry components | No |
| `sdkconfig.old` | Backup of the previous config | No |

## 5. Foundation projects: freertos\_test, dht11\_temperature, hw\_verify

These three are the building blocks. Run them in this order on a new board: toolchain first, then the sensor, then the full wiring check.

### 5.1 freertos\_test

Proves the toolchain, flashing and serial monitor work. `hello_world_main.c` prints "Hello world!", the chip model, core count, silicon revision, flash size and minimum free heap. It then counts down 10 s and calls `esp_restart()`.

```bash
cd freertos_test && idf.py -p /dev/ttyUSB0 flash monitor
```

Expected: `Hello world!`, then `This is esp32 chip with 2 CPU core(s)...` and a countdown to restart.

### 5.2 dht11\_temperature

Reads the DHT11 on GPIO4 every 2 s from a FreeRTOS task (`dht11_task`) and logs `Temperature: 26 C | Humidity: 55 %`.

**How the DHT11 driver works (`dht11.c`):**

1. Drive the line LOW for 20 ms (start signal), then HIGH for 30 µs.
2. Switch the pin to input. The sensor answers LOW \~80 µs, then HIGH \~80 µs.
3. Read 40 bits. Each bit starts with a \~50 µs LOW; the length of the HIGH that follows sets the bit (\~26 µs = 0, \~70 µs = 1). The driver samples the line 40 µs after the HIGH starts.
4. Bytes = humidity, humidity decimal, temperature, temperature decimal, checksum. The checksum must equal the sum of the first four bytes.

Each wait gives up after 1,000 µs and returns `ESP_FAIL` with the stage (e.g. `Bit 38 LOW timeout`).

### 5.3 hw\_verify

Flash this whenever wiring changes. It checks every sensor and prints a PASS/FAIL line per part over serial.

| Check | How | Pass condition |
| --- | --- | --- |
| DHT11 | 5 reads, 2.5 s apart | `[DHT11] RESULT: 5 ok / 0 failed` |
| OLED | Probe every I2C address 0x03–0x77 on SDA21/SCL22 | `[OLED] Found device at 0x3C` |
| Mic | I2S at 16 kHz on WS33/SCK26/SD32, peak level every 300 ms | Peak > 500 and varying: `[MIC] RESULT: signal detected` |

Last run (with WS still on GPIO25): DHT11 5/5 at 26 °C / 55 %, OLED found at 0x3C, mic peaks 185,000–2,600,000 (24-bit). The firmware now expects WS on GPIO33 and hasn't been re-run since the move.

## 6. bluetooth\_test: SPP echo server

This project proves the phone can find, pair with and exchange data with the ESP32 over Classic Bluetooth. It shows up as `ESP32_SPP_ACCEPTOR` and sends back every byte it receives. Its pytest passed on hardware on 2026-09-24.

### Config (`sdkconfig.defaults`)

```text
CONFIG_BT_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y   # Classic only, no BLE
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_BLE_ENABLED=n
CONFIG_BT_SPP_ENABLED=y
```

### Startup sequence (`app_main`)

1. `nvs_flash_init()`. Bluedroid stores pairing keys in NVS; the partition is erased and re-initialised if full or outdated.
2. `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` frees BLE memory.
3. Controller init + enable in `ESP_BT_MODE_CLASSIC_BT`.
4. `esp_bluedroid_init_with_cfg()` + `esp_bluedroid_enable()` start the host stack.
5. Register the GAP callback (pairing) and SPP callback (data).
6. `esp_spp_enhanced_init()` in callback mode with L2CAP ERTM.
7. Security: SSP IO capability `ESP_BT_IO_CAP_IO` (display yes/no); legacy PIN set to variable.

```mermaid
sequenceDiagram
    participant P as Phone
    participant E as ESP32
    E->>E: SPP_INIT_EVT -> start server SPP_SERVER
    E->>E: SPP_START_EVT -> discoverable
    P->>E: Inquiry finds ESP32_SPP_ACCEPTOR
    P->>E: Pair (SSP confirm or PIN 1234)
    E-->>P: AUTH_CMPL_EVT success
    P->>E: RFCOMM connect (SRV_OPEN_EVT)
    P->>E: "hello" (DATA_IND_EVT)
    E-->>P: "hello" echoed
```

Only when the SPP server has started does the ESP32 name itself and turn on discoverable mode, so the phone never sees a half-ready device.

### Pairing

| Method | When | ESP32 behaviour |
| --- | --- | --- |
| Secure Simple Pairing, numeric comparison | Modern phones | `CFM_REQ_EVT`, auto-confirms; accept on phone |
| Legacy PIN | Old devices, SSP unsupported | Replies PIN `1234` (16 zeros if 16 digits required) |

### Test from a phone

1. Flash, wait for `Discoverable as 'ESP32_SPP_ACCEPTOR'`.
2. Pair from the phone's Bluetooth settings.
3. Open a serial Bluetooth terminal app, connect, send text; the same text comes back.

### Automated test

`pytest_bt_spp_test.py` flashes the board and waits for two lines logged in a fixed order: `SPP server started`, then `Discoverable as 'ESP32_SPP_ACCEPTOR'`.

```bash
cd bluetooth_test
pytest pytest_bt_spp_test.py --embedded-services esp,idf --target esp32 --port /dev/ttyUSB0
```

## 7. sensor\_hub: DHT11 → OLED → Bluetooth pipeline

Every 5 s the sensor hub reads the DHT11, shows the reading on the OLED, and pushes it to a connected phone as `T:26C H:57%`. A phone can also send `GET` for an immediate reading. With the DHT11 fix in place, 12 of 13 reads succeeded over 65 s on hardware (before the fix: 0 of 8).

```mermaid
flowchart LR
    S[DHT11<br/>GPIO4] -->|every 5 s| T[sensor_task<br/>prio 5]
    T -->|LVGL lock| U[ui.c labels]
    U --> L[LVGL task<br/>prio 2] --> O[SSD1306<br/>I2C 0x3C]
    T -->|spp mutex| W[esp_spp_write]
    W --> P[Phone<br/>SPP terminal]
    P -->|GET| C[esp_spp_cb] --> W
```

The sensor task drives both outputs; the SPP callback answers `GET` on its own by doing a fresh read.

### Config and dependencies

- `sdkconfig.defaults`: Classic BT + SPP, no BLE; `CONFIG_LV_CONF_SKIP=y` (LVGL configured from Kconfig), `LV_USE_OBSERVER`, `LV_USE_SYSMON`.
- `main/idf_component.yml`: `lvgl/lvgl: "9.2.0"`, downloaded into `managed_components/` on first build.
- `PRIV_REQUIRES`: `bt nvs_flash esp_driver_gpio esp_driver_i2c esp_lcd esp_timer`.

### Startup (`app_main`)

1. Create `spp_state_mutex`.
2. `dht11_init(GPIO4)`: input/output pin with pull-up.
3. `oled_init()`: I2C master bus (port 0, 400 kHz, internal pull-ups) → `esp_lcd` SSD1306 panel (128×64, 1 bpp) → LVGL display in I1 (monochrome) format, full-frame render → 5 ms `esp_timer` tick → LVGL task → `sensor_ui_init()`.
4. `bt_init()`: NVS, controller (Classic), Bluedroid, GAP + SPP callbacks, SPP server `SPP_SERVER`, discoverable as `ESP32_SENSOR_HUB`.
5. Start `sensor_task` (4 KB stack, priority 5).

### Tasks and shared state

| Task / context | Does | Protects shared state with |
| --- | --- | --- |
| `sensor_task` | Read DHT11, update UI, send over SPP, sleep 5 s | LVGL lock, `spp_state_mutex` |
| LVGL task (`lvgl_port_task`) | `lv_timer_handler()`, 5–500 ms sleep | `lvgl_api_lock` |
| `esp_timer` callback | `lv_tick_inc(5)` | none needed |
| Bluedroid callback task | GAP pairing, SPP connect/close/data, `GET` | `spp_state_mutex`, LVGL lock |

`spp_client_handle` (0 = no client) is the only BT state shared between tasks; every read or write goes through the mutex.

### OLED screen (`ui.c`)

Three LVGL labels: `ESP32 Sensor Hub` / `26C   57%RH` (or `sensor error`) / `BT: connected` or `BT: waiting`.

### Failure tolerance

A failed read keeps showing and sending the last good value. Only after 3 failures in a row (`SENSOR_MAX_CONSECUTIVE_FAILURES`) does the output switch to `sensor error` / `SENSOR_ERROR`.

### The DHT11 fix (commit f96266f)

With Bluetooth and LVGL running, interrupts arrived in the middle of the \~4 ms bit frame, and the driver missed the \~50 µs LOW pulses at the end. Every read failed with `Bit 38/39 LOW timeout`, while the same sensor passed 5/5 under `hw_verify`.

- **Fix:** the response + 40-bit read runs inside `portENTER_CRITICAL(&dht11_spinlock)`; errors are logged only after `portEXIT_CRITICAL`.
- **Cost:** interrupts on that core are masked for \~4 ms every 5 s; Bluetooth stayed stable in testing.
- **Better long-term option:** read the DHT11 with the RMT peripheral, which needs no interrupt masking.

### SPP protocol

| Direction | Message | Meaning |
| --- | --- | --- |
| ESP32 → phone | `T:26C H:57%\r\n` | Periodic reading (every 5 s) |
| ESP32 → phone | `SENSOR_ERROR\r\n` | 3+ consecutive failed reads |
| Phone → ESP32 | `GET` (any case, CR/LF trimmed) | Immediate reading |
| Phone → ESP32 | anything else | Echoed back as `echo: ...` |

Pairing is the same as `bluetooth_test`: SSP auto-confirm, legacy PIN `1234`.

## 8. hfp\_mic\_test: Bluetooth hands-free with mic and speaker

The ESP32 acts as a car-kit style hands-free device for the POCO F4. It carries the call: the INMP441 mic feeds your voice into the call, and GPIO25 plays the other person's voice. The mic path was verified on a real 56 s call (the other person heard you clearly). The speaker path is built but untested, pending an amplifier.

### Profiles running at once

| Profile | ESP32 role | Used for | Source file |
| --- | --- | --- | --- |
| HFP 1.x | Hands-Free (HF client) | Calls: ring, answer, audio both ways | `bt_app_hf.c` |
| A2DP | Sink | Music from the phone (bytes counted, not played) | `bt_app_av.c` |
| AVRCP | Controller | Play/pause/next/prev, track title/artist/album | `bt_app_av.c` |
| PBAP | Client (PCE) | Pull the phone book (`telecom/pb.vcf`) | `bt_app_pbac.c` |

### Key configuration (`sdkconfig.defaults`)

```text
CONFIG_BT_ENABLED=y  CONFIG_BT_BLE_ENABLED=n  CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y
CONFIG_BTDM_CTRL_BR_EDR_MAX_SYNC_CONN=1     # one SCO/eSCO voice link
CONFIG_BT_BLUEDROID_ENABLED=y  CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_HFP_ENABLE=y  CONFIG_BT_HFP_CLIENT_ENABLE=y
CONFIG_BT_PBAC_ENABLED=y  CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI=y         # voice over HCI to the app, not PCM pins
CONFIG_EXAMPLE_PEER_DEVICE_NAME="POCO F4"
CONFIG_EXAMPLE_PEER_DEVICE_ADDR="ac:1e:9e:f2:b1:c0"
```

`CONFIG_BT_HFP_WBS_ENABLE=y` (wideband speech) is on in `sdkconfig`, so phones negotiate mSBC (16 kHz). `CONFIG_BT_HFP_USE_EXTERNAL_CODEC` is off: Bluedroid encodes/decodes CVSD and mSBC itself.

### Kconfig options (`main/Kconfig.projbuild`, menu "HFP Example Configuration")

| Option | Default | Meaning |
| --- | --- | --- |
| `EXAMPLE_SSP_ENABLED` | y | Secure Simple Pairing; off = legacy PIN only |
| `EXAMPLE_PEER_DEVICE_NAME` | `ESP_HFP_AG` | Phone name to find by discovery |
| `EXAMPLE_PEER_DEVICE_ADDR` | empty | Phone address for direct connect; empty = discovery only; malformed = warning, ignored |
| `EXAMPLE_ENABLE_CONSOLE_REPL` | y | UART console with the commands below |

### Connection flow

1. `app_main`: NVS → controller (Classic) → Bluedroid → `bt_app_task_start_up()` (queue of 10, task `BtAppT`, 4 KB) → console REPL.
2. Stack-up event: GAP callback, HFP client init, PBAP init, A2DP sink + AVRCP init, device name `ESP_HFP_HF`, PIN `0000` fixed for legacy, connectable + discoverable.
3. If `EXAMPLE_PEER_DEVICE_ADDR` parses, call `esp_hf_client_connect()` on it straight away.
4. In parallel, start a 30-unit (\~38 s) general inquiry; on a result whose name equals `EXAMPLE_PEER_DEVICE_NAME`, cancel discovery and connect. Retries forever if not found.
5. HFP goes `connected` → `slc_connected` (service-level connection). The phone now lists the ESP32 as a call audio route; PBAP connects and pulls the phone book.
6. The phone connects A2DP + AVRCP; AVRCP fetches current track metadata.

```mermaid
sequenceDiagram
    participant E as ESP32 (HF)
    participant P as POCO F4 (AG)
    E->>P: ACL + RFCOMM connect (HFP)
    E->>P: AT commands (BRSF, CIND, CMER...)
    P-->>E: SLC connected, indicators
    P->>E: RING + CLIP (incoming call)
    Note over E,P: answered on phone
    P->>E: eSCO setup, codec mSBC
    E->>P: mic audio (16 kHz)
    P->>E: far-end audio (16 kHz)
    P->>E: eSCO closed (call ended)
```

All signalling rides RFCOMM AT commands; the voice itself uses a separate synchronous (eSCO) link that exists only during the call.

### Uplink: mic → call

```mermaid
flowchart LR
    M[INMP441<br/>I2S1 16 kHz 32-bit] --> F[mic_feed_task<br/>prio 6]
    F -->|>>16 to 16-bit| D{Codec?}
    D -->|mSBC| R[Ring buffer<br/>3600 B]
    D -->|CVSD: resample to 8 kHz| R
    R --> O[outgoing_cb<br/>Bluedroid pulls]
    O --> P[Phone → other person]
```

The mic task pushes PCM and calls `esp_hf_client_outgoing_data_ready()`; Bluedroid pulls fixed-size frames through `outgoing_cb` and encodes them.

### Downlink: call → speaker (untested)

```mermaid
flowchart LR
    P[Phone: other person] --> I[incoming_cb<br/>16-bit PCM]
    I -->|to 8-bit unsigned<br/>CVSD: repeat x2| Q[Ring buffer<br/>4096 B ~250 ms]
    Q --> T[spk_play_task<br/>prio 6]
    T --> A[DAC ch0 GPIO25<br/>16 kHz, APLL clock]
    A --> S[Amp / AUX speaker]
```

The callback never blocks: if the queue is full, audio is dropped with a rate-limited warning. On underrun the task writes mid-scale (128) silence.

### Audio lifecycle

| Event | Action |
| --- | --- |
| Audio state `connected` (CVSD) or `connected_msbc` | Register data callbacks, lazy-init I2S mic and DAC once, set `s_audio_wideband`, set `s_audio_active` |
| During call | Mic task and speaker task run; buffers flow |
| Audio state `disconnected` | Clear `s_audio_active`, drain both ring buffers; hardware stays initialised for the next call |
| Audio state `connected_lc3` | Logged as an error: LC3 needs the external-codec path |

### Hardware constraints behind the design

- **DAC pins:** only GPIO25 and GPIO26 can output from the DAC, which is why the mic's WS moved to GPIO33.
- **I2S0 is taken:** the DAC's DMA always uses I2S0 on ESP32, so the mic is pinned to `I2S_NUM_1`.
- **DAC clock:** the default clock can't go below 19.6 kHz, so the DAC uses the APLL at exactly 16 kHz.
- **Resolution:** the DAC is 8-bit, so expect AM-radio quality.

### Console commands (UART, prompt `hfp_hf>`)

| Command | Action |
| --- | --- |
| `con` / `dis` | Connect / release the HFP link |
| `cona` / `disa` | Open / close the audio (voice) link |
| `ac` / `rc` | Answer / reject an incoming call |
| `d <num>` / `rd` / `dm` | Dial a number / redial / dial memory |
| `qop` / `qc` | Query operator / current calls |
| `vron` / `vroff` | Start / stop voice recognition |
| `vu` | Volume update |
| `rs` / `rv` / `rh` | Subscriber info / last voice tag / response-and-hold |
| `k <dtmf>` | Send a DTMF tone (0–9, \*, #, A–D) |
| `xp` / `bat` | Apple XAPL battery reporting / send battery level |
| `acon` / `adis` | Connect / disconnect A2DP |
| `play` / `pause` / `next` / `prev` | AVRCP transport control |
| `md` | AVRCP: request title/artist/album |

## 9. Bluetooth profiles and protocols reference

Every Bluetooth feature here is Classic Bluetooth (BR/EDR); BLE is disabled in all projects to save memory. The ESP32 runs Espressif's controller firmware plus the Bluedroid host stack, which talk over an internal HCI.

### Protocol stack

```text
+--------------------------------------------------------------+
| Your app: main.c, bt_app_hf.c, bt_app_av.c, bt_app_pbac.c   |
+----------------+-----------+------------+-------------+-----+
| SPP            | HFP (HF)  | A2DP sink  | AVRCP CT    | PBAP|  profiles
+----------------+-----------+------------+-------------+-----+
| RFCOMM (serial port emulation)  |  AVDTP     |  AVCTP  | OBEX |
+---------------------------------+------------+---------+------+
| L2CAP (channels, ERTM)     SDP (service discovery)           |
+--------------------------------------------------------------+
| GAP / security: inquiry, pairing (SSP / legacy PIN), bonding |
+--------------------------------------------------------------+
| HCI (host <-> controller; also carries SCO voice data here)  |
+--------------------------------------------------------------+
| Controller: baseband, link manager, radio (2.4 GHz)          |
+--------------------------------------------------------------+
```

### Terms

| Term | What it is | Where it matters here |
| --- | --- | --- |
| GAP | Discovery, connection and security rules | Device names, discoverable mode, inquiry for the phone |
| SSP | Secure Simple Pairing (BT 2.1+); numeric comparison or passkey | All projects auto-confirm numeric comparison |
| Legacy PIN | Pre-2.1 pairing with a shared PIN | `1234` (SPP projects), `0000`/`1234` (HFP) |
| Bonding | Pairing keys saved to NVS | Survives reflash; stale keys break pairing → forget + `erase-flash` |
| L2CAP | Multiplexed channels over one ACL link | ERTM enabled for SPP |
| SDP | Lists services a device offers | Phone finds SPP server / HFP service |
| RFCOMM | Serial-port emulation over L2CAP | SPP data; HFP AT commands |
| SPP | Serial Port Profile | `bluetooth_test`, `sensor_hub` |
| ACL link | Asynchronous data link | Everything except voice |
| SCO / eSCO | Synchronous voice links with reserved slots | Call audio; max 1 (`MAX_SYNC_CONN=1`) |
| HFP | Hands-Free Profile: AG = phone, HF = ESP32 | `hfp_mic_test` |
| SLC | Service Level Connection: HFP's AT-command session | Needed for the ESP32 to appear as a call audio route |
| CVSD | Narrowband voice codec, 8 kHz | Fallback; mic resampled 16 → 8 kHz |
| mSBC | Wideband voice codec, 16 kHz ("HD voice") | Negotiated by the POCO F4 |
| LC3-SWB | Super-wideband codec (HFP 1.9) | Not supported without external codec |
| A2DP / SBC | Stereo music streaming, SBC codec | Phone → ESP32 music (counted only) |
| AVDTP | Transport for A2DP | Stream setup |
| AVRCP / AVCTP | Remote control + metadata | play/pause, track title |
| PBAP / OBEX | Phone book access over object exchange | Pulls `telecom/pb.vcf` |
| HCI audio data path | SCO voice delivered to the app over HCI | `CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI=y` |

### Key HFP AT commands (phone = AG, ESP32 = HF)

| AT command | Direction | Purpose |
| --- | --- | --- |
| `AT+BRSF` | HF → AG | Exchange supported features |
| `AT+BAC` | HF → AG | Offer codecs (CVSD, mSBC) |
| `AT+CIND` / `AT+CMER` | HF → AG | Read and subscribe to indicators (call, signal, battery) |
| `RING` / `+CLIP` | AG → HF | Incoming call + caller number |
| `ATA` / `AT+CHUP` | HF → AG | Answer / hang up |
| `ATD<num>;` | HF → AG | Dial |
| `+BCS` / `AT+BCS` | both | Codec selection before audio opens |
| `AT+VGS` / `AT+VGM` | both | Speaker / mic gain |
| `AT+VTS` | HF → AG | DTMF tone |

### Codec parameters

| Codec | Sample rate | Frame | Where encoded |
| --- | --- | --- | --- |
| CVSD | 8 kHz, 16-bit mono PCM in | 60/120 bytes per packet | Bluedroid (internal) |
| mSBC | 16 kHz, 16-bit mono PCM in | 120 samples (240 bytes) per 7.5 ms | Bluedroid (internal) |

## 10. Command reference

Run every command from inside a project folder after `source esp-idf/export.sh` (use the correct relative path). The port is `/dev/ttyUSB0`.

### Build, flash, monitor

| Command | What it does |
| --- | --- |
| `idf.py set-target esp32` | Pick the chip (once per project; wipes `build/`) |
| `idf.py build` | Compile; output in `build/<project>.bin` |
| `idf.py -p /dev/ttyUSB0 flash` | Write bootloader, partition table, app |
| `idf.py -p /dev/ttyUSB0 -b 115200 flash` | Slower, more reliable flash (use if the serial link drops) |
| `idf.py -p /dev/ttyUSB0 monitor` | Serial console; `Ctrl+]` exits |
| `idf.py -p /dev/ttyUSB0 flash monitor` | Both in one step |
| `idf.py menuconfig` | Edit config (e.g. HFP Example Configuration) |
| `idf.py size` | Show app and memory size |
| `idf.py fullclean` | Delete all build output |
| `idf.py -p /dev/ttyUSB0 erase-flash` | Wipe the whole chip, including NVS pairing keys |

### Build everything at once

```bash
source esp-idf/export.sh
for p in freertos_test dht11_temperature hw_verify bluetooth_test sensor_hub hfp_mic_test; do
  (cd $p && idf.py build > /tmp/build_$p.log 2>&1; echo "$p: exit $?")
done
```

### Capture serial output to a file (no interactive monitor)

Used for all hardware test runs; it resets the board, then records for N seconds.

```python
# cap.py  usage: python3 cap.py /dev/ttyUSB0 <seconds> <outfile>
import serial, sys, time
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False   # hard reset
end = time.time() + secs
with open(out, "wb") as f:
    while time.time() < end:
        f.write(s.read(4096)); f.flush()
```

```bash
python3 cap.py /dev/ttyUSB0 60 run.log
sed 's/\x1b\[[0-9;]*m//g' run.log | grep -a SENSOR_HUB   # strip colours, filter
```

### Automated tests

```bash
pip install pytest-embedded pytest-embedded-idf pytest-embedded-serial-esp
cd bluetooth_test
pytest pytest_bt_spp_test.py --embedded-services esp,idf --target esp32 --port /dev/ttyUSB0
```

Logs land in `/tmp/pytest-embedded/<timestamp>/.../dut.log`.

### Git

| Command | Use |
| --- | --- |
| `git switch -c <branch>` | Start work off `master` |
| `git switch master && git merge --ff-only <branch>` | Bring work in without a merge commit |
| `git push origin master` | Publish |
| `git branch -d <branch>` | Remove a merged branch |

## 11. Testing and verification process

Testing goes from cheapest to most real: compile everything, check the wiring, run each pipeline on the board, then finish with a real phone call. Each stage must pass before the next is worth running.

```mermaid
flowchart LR
    A[1. Build all 6<br/>projects] --> B[2. hw_verify<br/>wiring check]
    B --> C[3. sensor_hub<br/>on board]
    C --> D[4. bluetooth_test<br/>pytest]
    D --> E[5. hfp_mic_test<br/>boot check]
    E --> F[6. Real call<br/>with phone]
```

### Procedure

1. **Build:** all six projects must build with 0 errors and 0 warnings.
2. **Wiring:** flash `hw_verify`; all three RESULT lines must pass.
3. **Sensor pipeline:** flash `sensor_hub`, capture 60 s; expect SPP server up, discoverable, and most reads OK (the first read during BT startup may fail).
4. **SPP:** run the `bluetooth_test` pytest.
5. **HFP boot:** flash `hfp_mic_test`; expect a single boot, all profiles `Init Complete`, direct connect attempt to the configured address.
6. **Real call:** keep a serial capture running, pair the phone, place or answer a call. Check for `audio state connected_msbc`, `mic: streaming to call at 16kHz (mSBC)`, no `rb send fail`, no panic. Ask the other person how it sounds.

### Results on record (2026-09-24)

| Stage | Result | Evidence |
| --- | --- | --- |
| Real call, mic uplink | Pass: other person heard clearly | POCO F4 incoming call, mSBC, \~56 s, 0 overruns, 0 crashes |
| hfp\_mic\_test boot | Pass | 1 boot, HFP/A2DP/AVRCP init complete |
| bluetooth\_test pytest | Pass | 1 passed in 12.17 s |
| sensor\_hub, after DHT11 fix | Pass | 12/13 reads over 65 s; later runs 5/6 and 4/5 |
| sensor\_hub, before fix | Fail | 0/8 reads, `Bit 38/39 LOW timeout` |
| hw\_verify | Pass | DHT11 5/5, OLED at 0x3C, mic signal detected |
| Build all 6 | Pass | 0 errors, 0 warnings |

### Not yet tested

- [ ] Speaker downlink on GPIO25 (needs amp or powered AUX speaker); look for `spk: DAC ready on GPIO25 at 16000 Hz`
- [ ] Mic on its new WS pin, GPIO33 (re-run `hw_verify`, then a call)
- [ ] `sensor_hub` with a phone: pairing, 5 s pushes, `GET`
- [ ] OLED contents (screen not visible from the test host)
- [ ] Outgoing call from the ESP32 console (`d <num>`)

## 12. Git workflow and history

Work happens on a short-lived branch, gets fast-forwarded into `master`, is pushed to GitHub, and then the branch is deleted. Commit messages record what was tested on hardware and what wasn't.

```mermaid
flowchart LR
    A[master] --> B[git switch -c topic]
    B --> C[edit + build<br/>+ test on board]
    C --> D[commit<br/>with test notes]
    D --> E[switch master<br/>merge --ff-only]
    E --> F[push origin master]
    F --> G[branch -d topic]
```

Fast-forward merges keep history linear with no merge commits.

### Commit history (newest first)

| Date | Commit | Change |
| --- | --- | --- |
| 2026-09-24 | `419c564` | hw\_verify: mic WS on GPIO33, document speaker wiring |
| 2026-09-24 | `9c87da4` | hfp\_mic\_test: play far-end call audio on the GPIO25 DAC (untested) |
| 2026-09-24 | `60f9a99` | hfp\_mic\_test: target the POCO F4 instead of the Narzo 50A |
| 2026-09-24 | `9d5f0ca` | hfp\_mic\_test: feed mic at 16 kHz for mSBC calls, fix hang-up race (verified on a call) |
| 2026-09-24 | `092af97` | hfp\_mic\_test: move peer phone address into Kconfig |
| 2026-09-24 | `3b9d4a4` | bluetooth\_test: rename CMake project from bt\_discovery |
| 2026-09-24 | `d9a3753` | bluetooth\_test: update pytest for the SPP acceptor |
| 2026-09-24 | `1588817` | bluetooth\_test: rewrite README for the SPP echo server |
| 2026-09-24 | `2208c63` | README: list sensor\_hub and hfp\_mic\_test |
| 2026-09-24 | `91af402` | Add hfp\_mic\_test |
| 2026-09-24 | `56a721f` | bluetooth\_test: discovery demo → SPP acceptor with pairing |
| 2026-09-24 | `f96266f` | sensor\_hub: read DHT11 frame inside a critical section |
| 2026-09-24 | `0a9e79b` | Add sensor\_hub pipeline |
| 2026-09-23 | `109ca3d` | Add root README |
| 2026-09-23 | `ba8ae1a` | Initial commit of ESP32 FreeRTOS test projects |

## 13. Known issues, troubleshooting, next steps

The most urgent open item is testing the speaker once the PAM8403 amplifier arrives. After that come flash space (`hfp_mic_test` has 9% left) and moving the DHT11 driver off interrupt masking.

### Known issues

| Issue | Impact | Mitigation |
| --- | --- | --- |
| `hfp_mic_test` app is 91% of its 1 MB partition | Little room for new features | Bigger partition table (4 MB chip, header says 2 MB) or trim features |
| DHT11 read masks interrupts \~4 ms every 5 s | Possible BT hiccups | Move to RMT peripheral |
| DHT11 fix only in `sensor_hub` | Other copies fail if BT is added | Share one driver component |
| First DHT11 read fails during BT startup | Cosmetic | Absorbed by 3-failure tolerance |
| 8-bit DAC | AM-radio-level sound | MAX98357A I2S amp (firmware change) |
| Phone Bluetooth addresses committed to a public repo | Minor privacy | Move to an untracked config; history keeps old values |
| Some serial log lines garbled | Cosmetic | None yet |
| Image header 2 MB vs 4 MB flash detected | Warning at boot | Set flash size to 4 MB in `menuconfig` |

### Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Flash fails: "No more data to read from the serial port" | USB link dropped mid-write | Retry with `-b 115200`; shorter/better cable |
| Permission denied on `/dev/ttyUSB0` | User not in `dialout` | `sudo usermod -aG dialout $USER`, log in again |
| Pairing failed, status N | Stale bonding keys | Forget device on phone; `idf.py erase-flash` |
| ESP32 connects to the wrong phone | Old name/address in config | Set `EXAMPLE_PEER_DEVICE_NAME` / `_ADDR`, rebuild |
| DHT11 `Bit 38/39 LOW timeout` | Interrupts during bit frame | Use the critical-section driver from `sensor_hub` |
| Other person hears choppy, fast voice | Mic fed at 8 kHz on an mSBC call | Fixed in `9d5f0ca` |
| No mic audio after rewiring | WS not on GPIO33 | Move WS jumper; run `hw_verify` |
| Hum from speaker | Ground loop | Power speaker/amp from the ESP32's USB source |
| Bluetooth not selectable during a call | Only paired, no SLC | Wait for `slc_connected` in the log; check peer config |

### Next steps

- [ ] Buy a PAM8403 amplifier; wire GPIO25 → 1 µF → L-in, GND, 5V, speaker on L+/L−
- [ ] Move mic WS to GPIO33 and re-run `hw_verify`
- [ ] Real call with serial capture: confirm speaker and re-pinned mic
- [ ] Commit results; update this doc's section 11
- [ ] Optional: RMT-based DHT11 driver shared across projects
- [ ] Optional: enlarge the app partition for `hfp_mic_test`
