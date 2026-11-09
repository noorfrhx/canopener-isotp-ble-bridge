# ISO-TP BLE bridge for the CAN Opener SE

A port of [Switchleg1/esp32-isotp-ble-bridge](https://github.com/Switchleg1/esp32-isotp-ble-bridge) (branch `BridgeLEG`, itself a fork of bri3d's original) from the Macchina A0 (ESP32) to the **CAN Opener SE** (ESP32-C5).

It turns the CAN Opener into a Bluetooth LE to ISO-TP adapter that works with:

- **SimosTools** (Android): flashing and logging for Simos 18 ECUs and DSG gearboxes
- **VW_Flash** (Python, Mac/PC): use its BLE interface

The bridge has preset links for the ECU (0x7E0/0x7E8), TCU (0x7E1/0x7E9), Haldex (0x7E5/0x7ED) and the broadcast DTC address. It advertises as `BLE_TO_ISOTP20`.

> **Status: compiles, not yet tested on hardware.** Nobody has run this on a real CAN Opener SE or a car yet. Test it on the bench or with read-only operations (logging, reading part numbers) before you flash anything.

## What changed from the A0 version

| Area | Macchina A0 | CAN Opener SE |
|---|---|---|
| Chip | ESP32 (Xtensa, dual core) | ESP32-C5 (RISC-V, single core) |
| CAN driver | legacy `driver/twai.h` | new `esp_twai` node API (the C5 has TWAI-FD controllers, which the legacy driver does not support). See `main/twai.c` |
| CAN pins | TX 5 / RX 4 | TX 1 / RX 0 (CAN 1, OBD-II pins 6/14) |
| Transceiver control | "S" pin on GPIO 21, low | TCAN3414 STB on GPIO 3, low |
| Status LED | WS2812 on GPIO 2 via legacy RMT, power enable on GPIO 13 | WS2812 on GPIO 2 via the `espressif/led_strip` component, no enable pin |
| ESP-IDF | 4.3 | 5.5 |
| Console | UART0 | USB Serial/JTAG (keeps UART0 free) |
| Logging | off | warnings only |

The pin map is in `main/board.h`. The ISO-TP, BLE protocol and persist/logging code are unchanged, apart from compile fixes for newer GCC (header globals made `extern`, uninitialized struct warnings, missing includes).

## Build

You need ESP-IDF 5.5 in `~/esp/esp-idf` (or set `IDF_PATH`), plus `cmake` and `ninja` (`brew install cmake ninja`).

```bash
./build.sh
```

ESP-IDF can't build from a path with spaces, so the script copies the project to `~/.cache/canopener-bridge` and builds there. The binaries land in `dist/`.

## Flash

Plug the CAN Opener into your computer over USB-C (not into the car), then:

```bash
./build.sh flash
```

If the port isn't found, hold **BOOT**, tap **RESET**, release BOOT and try again. To flash the prebuilt files in `dist/` without building:

```bash
esptool.py --chip esp32c5 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x2000 dist/bootloader.bin 0x8000 dist/partition-table.bin 0x10000 dist/canopener_isotp_bridge.bin
```

This replaces the stock CAN Opener firmware. To go back to stock, use https://dev.canopener.io/flash (model CO-SE-NEXUS).

## First test on the device

1. Flash it with USB plugged in. The LED should turn dim red and the bridge should show up over Bluetooth as `BLE_TO_ISOTP20`. Release builds only log warnings; for startup logs, set `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` in `sdkconfig.defaults`, run `./build.sh flash` and look for `TWAI: Driver installed (500000 bit/s)`.
2. Plug it into the car (ignition on, engine off). Connect from SimosTools or VW_Flash and read ECU info: part number, software version, VIN.
3. Only after reads work reliably, think about flashing. Keep a battery charger on the car the whole time you flash.

## Known gaps

- **Untested on hardware.** In particular, the TX path keeps a small ring of frames for the new driver (`twai.c`), and bus-off recovery hasn't been tested on a live bus.
- **Bus-off mid-transfer:** frames already in the controller when the bus goes off are lost (logged as "failed to send"), and frames still queued go out after recovery. The ECU will then reject that ISO-TP transfer. The legacy firmware also lost frames here; either way, a bus-off during a flash means retrying that step.
- **Frame spacing:** up to 32 frames can be queued in the driver, so on a busy bus consecutive frames may go out closer together than the ISO-TP STmin asked for. The diagnostic CAN on the OBD port is normally quiet.
- **Wired (UART) mode** still uses UART0. On the SE, the USB-C port is most likely the C5's native USB, so the wired protocol isn't reachable over USB-C yet. BLE is the supported path.
- **CAN 2** (OBD-II pins 3/11) is not used.
