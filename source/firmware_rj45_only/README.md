# NetworkBox SLIP / W5500 test

Separate Pico 2 W firmware for testing the Atari SLIP link through the W5500.
The validated Wi-Fi firmware remains in `../networkbox_slip_wifi` and is not
modified by this project.

## Wiring

- Atari RS-232 through MAX3232: Pico GP0 TX, GP1 RX, 19200 baud, 8N1.
- W5500 SPI0: MISO GP16, CS/SS GP17, SCK GP18, MOSI GP19, RESET GP20.
- Connect Ethernet to the same LAN/router that provides DHCP before powering
  the Pico. The firmware waits for physical link and then requests DHCP.

## Test

1. Flash `releases/networkbox_slip_w5500_ping_test.uf2` to the Pico 2 W.
2. Open the Pico USB serial monitor at 115200 baud.
3. Wait for `W5500 Ethernet link up`, then for `NetworkBox active - W5500 IP ...`.
4. Start STinG on the Atari and run the existing `PING.PRG` against the router
   (usually `192.168.1.254`) first. Once that works, try the PC IP.
5. Keep the Wi-Fi UF2 as the rollback option; this W5500 image has not yet been
   validated on the physical Network Box.

## Build

Open this directory as a separate Pico project in VS Code and build
`networkbox_slip_w5500`. It uses the local Pico SDK 2.3.1 and the WIZnet
ioLibrary_Driver plus WIZnet's lwIP MACRAW adapter. The adapter's SPI pins are
the same as the wiring listed above.
