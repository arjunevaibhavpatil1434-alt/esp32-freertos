# Bluetooth SPP Echo Test

Classic Bluetooth SPP (Serial Port Profile) server for checking that a phone
can find, pair with, and talk to the ESP32 over Bluetooth. The ESP32 shows up
as `ESP32_SPP_ACCEPTOR` and sends back any data it receives, so you can
confirm data flows both ways.

This is the plain Bluetooth building block that [`sensor_hub`](../sensor_hub/)
builds on: it uses the same pairing and SPP setup, without the sensor and OLED.

## What it does

- Starts the Classic BT controller and the Bluedroid stack (BLE memory is released; Classic only)
- Registers an SPP server named `SPP_SERVER` that requires authentication
- Makes the ESP32 connectable and discoverable as `ESP32_SPP_ACCEPTOR`
- Handles both pairing methods:
  - **Secure Simple Pairing** (modern phones): numeric comparison is
    auto-confirmed on the ESP32 side, so just accept the prompt on the phone
  - **Legacy PIN pairing** (older devices): PIN is **`1234`**
- Logs every received packet as a hex dump and writes the same bytes back

## Hardware required

Just an ESP32 dev board. No external wiring is needed. You also need a phone
with a Bluetooth serial terminal app (e.g. "Serial Bluetooth Terminal" on
Android).

## Building and flashing

```bash
source /home/server/esp32-freertos/esp-idf/export.sh
cd /home/server/esp32-freertos/bluetooth_test
idf.py set-target esp32
idf.py -p /dev/ttyUSB0 flash monitor   # Ctrl+] to exit monitor
```

## Testing from a phone

1. Flash the firmware and wait for `Discoverable as 'ESP32_SPP_ACCEPTOR'`.
2. On the phone, open Bluetooth settings and pair with `ESP32_SPP_ACCEPTOR`.
   Accept the pairing prompt, or enter `1234` if you're asked for a PIN.
3. In the serial terminal app, connect to `ESP32_SPP_ACCEPTOR`.
4. Type something and send it. The same text should come straight back.

## Expected serial output

```
BT_SPP: SPP initialized, starting server 'SPP_SERVER'
BT_SPP: SPP server started, scn:1
BT_SPP: Discoverable as 'ESP32_SPP_ACCEPTOR' — pair to it from your phone
BT_SPP: Own address:[xx:xx:xx:xx:xx:xx]
...
BT_SPP: SSP numeric comparison, value:NNNNNN — auto-confirming
BT_SPP: Pairing succeeded with '<phone name>' [xx:xx:xx:xx:xx:xx]
BT_SPP: Client connected, handle:... addr:[xx:xx:xx:xx:xx:xx]
BT_SPP: Received 7 bytes on handle:...
BT_SPP: 0x3ffc....   68 65 6c 6c 6f 0d 0a                              |hello..|
```

## Troubleshooting

- **Pairing failed, status:N**: remove `ESP32_SPP_ACCEPTOR` from the
  phone's paired devices and pair again. Stale bonding keys from an earlier
  flash are the usual cause. To wipe the ESP32 side too, run `idf.py -p /dev/ttyUSB0 erase-flash`.
- **Phone pairs but the terminal app can't connect**: make sure the app
  connects to the paired device over classic Bluetooth (SPP/RFCOMM), not
  BLE.
