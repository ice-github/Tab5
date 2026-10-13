---
name: tab5-linux-build-deploy
description: Use when building or flashing the Tab5 firmware on a real Linux machine (not WSL): setting up ESP-IDF without sudo, identifying the ESP32-P4 device, backing up flash, configuring the chip-revision range, building, flashing, and verifying boot/playback. Trigger when the user asks to set up the build environment, compile, flash, or deploy to the Tab5.
---

# Tab5 Linux Build & Deploy (real Linux, ESP-IDF v5.5.x)

Verified on CachyOS (Arch-based), ESP32-P4 **rev v1.0**, 16MB flash,
IDF **v5.5.4**. User handles `sudo`; everything else stays in user space.
Never touch the OS Python/CMake; never enable the IDF export globally.

## 1. sudo part (user, one time)

```bash
sudo pacman -S --needed --noconfirm gperf ccache
sudo usermod -aG uucp na0ki   # serial group on this distro (or dialout)
```

Then **logout/login + restart the agent session** so the new group
applies. Gate: the agent process can write the port, e.g.
`test -w /dev/ttyACM0`.

Quick check-in for the current session only (lost on replug, not a
substitute for the group):

```bash
sudo setfacl -m u:na0ki:rw /dev/ttyACM0
```

## 2. User-space toolchain layout

| What | Where |
|---|---|
| ESP-IDF | `~/esp/esp-idf-v5.5.4/` (tag + `--recursive`) |
| IDF tools + Python env | `~/.espressif/` (`IDF_TOOLS_PATH` default) |
| Build Python 3.12 | `uv python install 3.12` (OS has 3.14; do not use it) |
| Host tools venv | `~/.venv-tab5-host` + `pyserial` (+ `esptool` for backup) |
| Flash backups | `backups/` (git-ignored) |
| Run logs/records | outside the repo (`~/tab5-records/`) |

Force Python 3.12 for `install.sh` (its detector accepts 3.9+ and would
otherwise pick the OS 3.14): put a shim dir first on `PATH` with
`python3 -> <uv python 3.12>`.

```bash
uv python install 3.12
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git \
  ~/esp/esp-idf-v5.5.4
export PATH="$HOME/esp/py312bin:$PATH"   # python3 -> 3.12 shim
cd ~/esp/esp-idf-v5.5.4 && ./install.sh esp32p4
./tools/idf_tools.py install cmake ninja  # IDF-pinned (3.30.2 / 1.12.1)
. ./export.sh                              # per-shell, never in profile
idf.py --version                           # ESP-IDF v5.5.4
riscv32-esp-elf-gcc --version              # esp-14.2.0
python --version                           # 3.12.x inside the env
```

Host self-test gate: `~/.venv-tab5-host/bin/python
tools/test_send_bundle.py` prints `all self-tests passed`.

## 3. Identify the device, preserve flash (before any write)

- Use the stable name, not `ttyACM0`:
  `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`.
- Confirm no monitor/sender holds the port.
- Read-only probe (resets the chip, writes nothing):

```bash
python -m esptool --port "$PORT" chip_id    # ESP32-P4, revision
python -m esptool --port "$PORT" flash-id   # detected flash size
```

- Full backup when readable (16MB took ~90s at 921600 baud), then record
  size + SHA-256 and the restore command (`write-flash 0x0 <file>`):

```bash
python -m esptool --port "$PORT" --baud 921600 \
  read-flash 0x0 0x1000000 backups/tab5-stock-<date>.bin
```

- Capture the boot log: existing bundle -> `player start`; missing
  bundle/flag -> single-shot `TAB5_XFER_READY`. Note: without SD or with
  an unreadable card the firmware reboots in a loop — fix the card, never
  auto-format (`CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL` stays off).
- Gate: chip + revision + flash size known, backup stored, or stop with
  the reason before flashing.

## 4. Revision config + dependency snapshot

Old silicon (rev v1.0) needs an explicit range on IDF v5.5.x
(`sdkconfig.defaults`):

```text
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

Gate on the **generated** `sdkconfig`, not the defaults file:
`ESP32P4_SELECTS_REV_LESS_V3=y`, `ESP_REV_MIN_FULL=100`,
`ESP_REV_MAX_FULL=199`, plus `FATFS_LFN_HEAP=y`, format-on-mount-fail
unset, `MAIN_TASK_STACK_SIZE=16384`, 16MB flash, esp32p4 target.

Record and commit the resolved component versions in
`firmware/dependencies.lock` (tracked in this project; known-good:
`m5stack_tab5 1.3.2`, `esp_lvgl_port 2.5.0`, `esp_codec_dev 1.5.11`).
See `esp32-p4-bringup` §1 for the revision rationale.

## 5. Build

```bash
. ~/esp/esp-idf-v5.5.4/export.sh && idf.py -C firmware build
```

Gate: exit 0, app `tab5_player.bin` fits the `factory 0x3F0000`
partition (known-good: ~650KB, 84% free), bootloader/app revision range
covers the probed chip, no `--force` anywhere. Record `.elf`/`.bin`
SHA-256 + sdkconfig + lockfile + source revision per build.

## 6. Flash + boot (separate steps, first time)

```bash
idf.py -C firmware -p "$PORT" flash     # uses build/flash_args, no erase
idf.py -C firmware -p "$PORT" monitor   # Ctrl+] to quit
```

Rules: no `erase-flash` in the normal path, no manual offsets, never run
monitor and the sender at once, re-check cable/port-ownership/permissions
before asking for BOOT/RESET button help.

Boot gate: this build's `tab5player build <date>` line, PSRAM init, SD
mount at HIGHSPEED, `player start` (or `TAB5_XFER_READY`), prefetch
primed, no panic/watchdog/reboot loop.

Playback gate (existing bundle): ~30fps sustained, `skip` stagnant,
`underrun=0` across the sample window, OSD pause/resume/brightness/
SCREEN OFF working. Screen/audio/touch appearance needs human eyes.

## 7. New bundle transfer (separate from flashing, only when needed)

Prefer the existing SD content. For a new bundle: verify on the PC
first (`tab5-media-bundle`), enter transfer mode (fresh boot without the
`complete` flag, or OSD `REPLACE` after confirming the old content may
go), stop the monitor, then send from the host venv and require per-file
`OK <CRC>` + final `COMPLETE` before reporting success.
