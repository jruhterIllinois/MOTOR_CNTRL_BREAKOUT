# SANE Backends 1.4.0 — Canon LiDE 220 (No-Motor Build)

SANE backends 1.4.0 modified to run the Canon LiDE 220 flatbed scanner with
motor movement disabled. Useful when operating the scanner without a functioning
motor (no return-to-home, no repositioning during calibration).

Also includes a GTK3 desktop frontend (`scanner_app`) for one-click scanning.

## Modifications to SANE backends

Three changes are made to `backend/genesys/gl124.cpp` and one to
`backend/genesys/tables_model.cpp` to disable all motor movement:

- **`tables_model.cpp`** — `ModelFlag::SHADING_REPARK` removed from the LiDE 220
  model flags, preventing the scan head from driving back home before/after
  white shading calibration.
- **`gl124.cpp` — `needs_home_before_init_regs_for_scan`** — returns `false`,
  skipping the pre-scan home requirement.
- **`gl124.cpp` — `move_back_home`** — made a no-op; the scanner never attempts
  to return to the home position.
- **`gl124.cpp` — `wait_for_motor_stop`** — made a no-op; motor stop polling is
  skipped entirely.

See `README.canon-lide220-no-motor.md` for the exact diffs.

## Building SANE backends

```bash
sudo apt-get install -y \
    build-essential autoconf automake autopoint libtool libtool-bin \
    pkg-config autoconf-archive gettext libusb-1.0-0-dev \
    libpng-dev libjpeg-dev libtiff-dev

autoreconf -fiv
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install
```

## scanner_app — GTK3 frontend

A single-file GTK3 frontend that wraps `scanimage`.

**Buttons:**

| Button | Action |
|---|---|
| Identify Scanner | Runs `scanimage -L` and detects the first USB scanner |
| Start Scan | Scans to `~/Desktop/root scans/<unix_timestamp>.pnm` |
| Stop Scan | Sends SIGTERM to the scanimage process |
| Reset Calibration | Clears the on-disk calibration cache (`--clear-calibration`) |

Scans are saved to `~/Desktop/root scans/` with the Unix timestamp as the
filename. The directory is created automatically on first scan.

The calibration cache is maintained by the SANE genesys backend between scans.
Use **Reset Calibration** to force a full recalibration on the next scan (useful
if scan quality degrades or scanner settings change).

### Building scanner_app

```bash
gcc $(pkg-config --cflags gtk+-3.0) \
    -o scanner_app scanner_app.c    \
    $(pkg-config --libs gtk+-3.0)   \
    -Wall -g
```

Requires `libgtk-3-dev`:
```bash
sudo apt-get install -y libgtk-3-dev
```

### Running

```bash
./scanner_app
```
