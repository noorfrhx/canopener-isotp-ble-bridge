# ISO-TP bridge for the CAN Opener SE

A port of [Switchleg1/esp32-isotp-ble-bridge](https://github.com/Switchleg1/esp32-isotp-ble-bridge) (branch `BridgeLEG`, itself a fork of bri3d's original) from the Macchina A0 (ESP32) to the **CAN Opener SE** (ESP32-C5).

It turns the CAN Opener into an ISO-TP adapter for:

- **VW_Flash** (Python, Mac/Linux/Windows) over **USB-C**: `--interface USBISOTP --usb_name /dev/cu.usbmodem…`. VW_Flash has had no Bluetooth support since 2023.
- **SimosTools** (Android) over **Bluetooth LE**. The bridge advertises as `BLE_TO_ISOTP20`.

The bridge has preset links for the ECU (0x7E0/0x7E8), TCU (0x7E1/0x7E9), Haldex (0x7E5/0x7ED) and the broadcast DTC address.

> **Status: desk-tested, not yet tested on a car.** On a real CAN Opener SE: VW_Flash connects over USB-C, 15/15 port open/close cycles without a reset, 4103-byte flash-sized packets are handled, and failed CAN sends are reported cleanly. It has since read identification, fault codes and 11 minutes of live data from a real Simos 18 ECU without dropouts; flash-sized transfers haven't been tested on a live CAN bus. Do read-only work on a car (identification, fault codes, logging) before you flash anything.

## What changed from the A0 version

| Area | Macchina A0 | CAN Opener SE |
|---|---|---|
| Chip | ESP32 (Xtensa, dual core) | ESP32-C5 (RISC-V, single core) |
| CAN driver | legacy `driver/twai.h` | new `esp_twai` node API (the C5 has TWAI-FD controllers, which the legacy driver does not support). See `main/twai.c` |
| CAN send | blocking, one frame at a time | same: `twai_send()` waits for the frame to leave and reports failures back to ISO-TP |
| CAN pins | TX 5 / RX 4 | TX 1 / RX 0 (CAN 1, OBD-II pins 6/14) |
| Transceiver control | "S" pin on GPIO 21, low | TCAN3414 STB on GPIO 3, low |
| Wired host link | UART0 via the A0's USB-serial chip | the C5's native USB Serial/JTAG, which is the USB-C port (`BOARD_WIRED_LINK_USB_SERIAL_JTAG` in `main/board.h`) |
| Console and logs | off | off (console, app and bootloader), so nothing else is ever written to the USB-C link |
| Bluetooth while USB is in use | stays available | turned off after the first USB packet, until power-cycle |
| ISO-TP response timeout | 100 ms, 1 flow-control wait | 1000 ms (ISO 15765-2), 10 waits |
| STmin | host override replaces the ECU's value | the larger of the host override and the ECU's value |
| Status LED | WS2812 on GPIO 2 via legacy RMT, power enable on GPIO 13 | WS2812 on GPIO 2 via the `espressif/led_strip` component, no enable pin |
| ESP-IDF | 4.3 | 5.5 |

The pin map is in `main/board.h`. The rest of the ISO-TP and BridgeLEG protocol code is as upstream, apart from compile fixes for newer GCC and the Bluetooth buffer bounds fix.

## Build

You need ESP-IDF 5.5 in `~/esp/esp-idf` (or set `IDF_PATH`), plus `cmake` and `ninja` (`brew install cmake ninja`).

```bash
./build.sh
```

ESP-IDF can't build from a path with spaces, so the script copies the project to `~/.cache/canopener-bridge` and builds there. The binaries land in `dist/`.

## Flash

Plug the CAN Opener into your computer over USB-C (not into the car). To flash the files in `dist/`:

```bash
esptool.py --chip esp32c5 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x2000 dist/bootloader.bin 0x8000 dist/partition-table.bin 0x10000 dist/canopener_isotp_bridge.bin
```

If the port isn't found, hold **BOOT**, tap **RESET**, release BOOT and try again.

This replaces the stock CAN Opener firmware. Back up the device first (`esptool.py read_flash 0 0x800000 backup.bin`). To go back to stock, write that backup back, or use https://dev.canopener.io/flash (model CO-SE-NEXUS).

## Using it with VW_Flash

```bash
uv run VW_Flash.py --interface USBISOTP --usb_name /dev/cu.usbmodem101 --action get_ecu_info
```

- On macOS, VW_Flash opens the port with DTR and RTS high, which doesn't reset the chip. Opening the port with DTR and RTS low does reset it, so don't open the port with other serial tools during a session.
- The chip's ROM still prints a short boot banner after any reset. Turning that off needs an eFuse burn, which this project doesn't do.
- VW_Flash's DQ381 mode sends an adapter setting (`0x9`) that this bridge doesn't implement. Don't flash a DQ381 gearbox with it.

## Known gaps

- **Not tested on a car or a live CAN bus with flash-sized transfers.**
- **Single core:** the ISO-TP processing task runs above the USB tasks. It no longer busy-waits during multi-frame transfers: it blocks until the next flow-control or consecutive frame arrives, and waits out STmin with a short delay. It hasn't been measured under a real flash yet.
- **USB reset:** opening the port with DTR and RTS low still resets the chip during a session. Setting `usb_uart_chip_rst_dis` after the first USB packet was tried and didn't stop it on the ESP32-C5, so it was taken out.
- **Bluetooth has no authentication.** If a phone connects before the first USB packet, the bridge sends everything to the phone instead. Unplug the bridge when you're not using it.
- **DQ381 adapter setting** (`0x9`) not implemented.
- **CAN 2** (OBD-II pins 3/11) is not used.
