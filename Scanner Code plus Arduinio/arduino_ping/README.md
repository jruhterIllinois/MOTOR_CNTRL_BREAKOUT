# arduino_ping

Liveness responder for the `[Connect Arduino]` button in `scanner_app.c`.

    host -> nano :  PING\n
    nano -> host :  PONG nanov3 <uptime_ms>\n

This link is a liveness check only. It is **not** motor control and is not
coupled to `scanimage` in any way.

## Flashing

The board on this bench is a CH340 clone (USB `1a86:7523`, `/dev/ttyUSB0`) but
ships with the **new** bootloader. Use `cpu=atmega328`. Selecting
`ATmega328P (Old Bootloader)` fails with `not in sync: resp=0x00` — that is the
failure mode, not the cure.

The Arduino IDE AppImage bundles an `arduino-cli`, so nothing needs installing.
Its mount path is ephemeral, so locate it fresh:

    CLI=$(ls -d /tmp/.mount_arduin*/resources/app/lib/backend/resources/arduino-cli | head -1)
    "$CLI" compile --fqbn arduino:avr:nano:cpu=atmega328 arduino_ping
    "$CLI" upload -p /dev/ttyUSB0 --fqbn arduino:avr:nano:cpu=atmega328 arduino_ping

Once flashed, the sketch lives in the Nano's flash. The IDE does not need to be
open, and the `.ino` does not need to exist, for the button to work.

Only one process may hold `/dev/ttyUSB0`. The IDE's **Serial Monitor** will take
it away from `scanner_app`; close the monitor before running the app.

## Tests

`test/test_serial.c` drives the real `arduino_open()` / `arduino_ping()` out of
`scanner_app.c` (it `#include`s the .c and renames `main`). `test/fake_nano.py`
stands up a pty that impersonates the board, so the retry logic can be exercised
without hardware.

    gcc $(pkg-config --cflags gtk+-3.0) -I. \
        -o /tmp/test_serial arduino_ping/test/test_serial.c \
        $(pkg-config --libs gtk+-3.0) -Wall -Wno-unused-function -g

Against a fake board — the three cases that matter:

    # 1. sketch answers at once
    PORT_FILE=/tmp/port.txt python3 arduino_ping/test/fake_nano.py 0 yes &
    SCANNER_APP_ARDUINO_PORT=$(cat /tmp/port.txt) /tmp/test_serial

    # 2. board still resetting for 1.6s (the DTR case) -> must still succeed
    PORT_FILE=/tmp/port.txt python3 arduino_ping/test/fake_nano.py 1.6 yes &

    # 3. port opens but no sketch flashed -> must time out cleanly, ~3.6s
    PORT_FILE=/tmp/port.txt python3 arduino_ping/test/fake_nano.py 0 no &

Against the real board:

    SCANNER_APP_ARDUINO_PORT=/dev/ttyUSB0 /tmp/test_serial

Measured on hardware: first connect ~605 ms (the `open()` yanks DTR and reboots
the Nano, so the first several pings are eaten by the bootloader), reconnect
~19 ms. `HUPCL` is cleared so DTR is held across `close()` and the board does not
reset again — confirmed by the Nano's reported uptime *increasing* between
connects rather than restarting near zero.
