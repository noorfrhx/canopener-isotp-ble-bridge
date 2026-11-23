#ifndef BOARD_H
#define BOARD_H

// CAN Opener SE (CO-SE), hardware revision v0.3 Rev.B, ESP32-C5-WROOM-1-N16R8.
// Source: github.com/the-can-opener/CAN-Opener-Hardware docs/hardware.md
//
// OBD-II pin 6/14 (the VW/Audi powertrain diagnostic CAN, where the ECU and
// TCU answer UDS) is wired to the board's "CAN 1" (net CAN_0) transceiver.
// "CAN 2" (net CAN_1) goes to OBD-II pins 3/11 and is not used by this bridge.

#define BOARD_NAME              "CAN Opener SE"

// CAN 1 -> OBD-II pins 6 (CAN-H) / 14 (CAN-L)
#define BOARD_CAN_TX_GPIO       1   // CAN_0_TXD
#define BOARD_CAN_RX_GPIO       0   // CAN_0_RXD

// CAN 2 -> OBD-II pins 3 / 11 (unused here)
#define BOARD_CAN2_TX_GPIO      8   // CAN_1_TXD
#define BOARD_CAN2_RX_GPIO      7   // CAN_1_RXD

// Shared STB line of both TCAN3414 transceivers.
// High = standby (receive-only wake detection), low = normal mode.
#define BOARD_CAN_STB_GPIO      3
#define BOARD_CAN_STB_ACTIVE    0

// Wired host link (BridgeLEG serial protocol, used by VW_Flash --interface USBISOTP).
// 1 = the ESP32-C5's native USB Serial/JTAG, which is the CAN Opener's USB-C port.
// 0 = UART0 on GPIO 11/12 (not reachable from outside the case).
// With 1, nothing else may write to USB Serial/JTAG: the console must be off
// (CONFIG_ESP_CONSOLE_NONE) or log lines would corrupt the binary stream.
#define BOARD_WIRED_LINK_USB_SERIAL_JTAG	1

// Addressable RGB status LED (WS2812-style), no separate power-enable pin.
#define BOARD_LED_GPIO          2

#endif
