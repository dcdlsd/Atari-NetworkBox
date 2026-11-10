# Experimental combined firmware at 38,400 baud

This is a copy of `../firmware_unified/networkbox_slip_wifi.cpp` with only
`BAUD_RATE` changed from `19200` to `38400`. `CMakeLists.txt` builds it for
Pico 2 W using the common source dependencies in `../firmware_unified/`.

The resulting UF2 is in `../../firmware/experimental/`. It is not validated
on the author's Atari STF; see the experimental firmware README for usage
and rollback instructions.
