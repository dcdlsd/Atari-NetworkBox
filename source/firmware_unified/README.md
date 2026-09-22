# NetworkBox unified test

This is an independent experimental project. The two validated projects
`networkbox_slip_wifi_setup` and `networkbox_slip_w5500` are not modified.

## Behaviour of v0.1

- If the RJ45 cable is connected when Pico starts, the W5500 is selected.
- Otherwise, Pico connects with the saved Wi-Fi configuration.
- If Wi-Fi credentials are missing or invalid, the existing setup page starts:
  `NetworkBox-Setup`, password `networkbox`, page `http://192.168.4.1`.
- There is deliberately no button or LED support in this first unified test.

The temporary access point is WPA2 protected. The saved password is not shown
by the page or in USB logs.

## Hardware test

1. Keep the validated Wi-Fi and W5500 UF2 files as backups.
2. Copy `releases/networkbox_unified_v0.1_boot_rj45_wifi_fallback.uf2` to Pico 2 W
   in BOOTSEL mode.
3. First test with RJ45 connected: the monitor must show `W5500 set as default`
   and then `NetworkBox active - W5500 OK`.
4. Validate Atari ping, then HighWire or FTP.
5. Reflash only if needed, unplug RJ45, and boot again. The monitor must show
   `Connecting to saved Wi-Fi...` followed by `NetworkBox active - Wi-Fi OK`.
6. Validate Atari ping, then HighWire or FTP again.

## Current scope

This test deliberately has no button, LEDs, live cable failover, or factory
reset. Those belong in the next unified firmware step.
