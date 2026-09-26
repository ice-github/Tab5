# agents.md — M5Stack Tab5 access via usbipd + WSL (`ubuntutmp`)

Target: `/home/ubuntu/workspace/tab5`
Device: M5Stack Tab5 (ESP32-P4 rev v1.0, 16MB flash, MAC `30:ed:a0:e2:96:93`)
USB ID: `303a:1001` (Espressif USB JTAG/serial debug unit)

## 1. Windows side (usbipd-win 5.3.0)

Confirm device (non-admin OK):

```powershell
usbipd list
usbipd state
```

Expected before attach: `1-2 303a:1001 ... Shared`, `ClientIPAddress: null`.
Expected after attach: `Attached`, `ClientIPAddress: 127.0.0.1`.

Attach (requires admin PowerShell):

```powershell
usbipd attach --wsl ubuntutmp --busid 1-2
```

Notes:
- `BUSID` (`1-2`) can change on replug; re-check with `usbipd list`. Alternative: `--hardware-id 303a:1001`.
- While attached, Windows COM9 is unavailable.
- Detach: `usbipd detach --busid 1-2`.

## 2. WSL side (`ubuntutmp`)

Device nodes:

```bash
ls -l /dev/ttyACM0 /dev/serial/by-id/
# /dev/ttyACM0
# /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:E2:96:93-if00 -> ../../ttyACM0
```

Permission: device is `root:dialout`. User `ubuntu` must be in `dialout`:

```bash
id  # must contain dialout
```

Fix (WSL `sudo` prompts for password; use root launch from Windows instead):

```powershell
wsl -d ubuntutmp -u root -- bash -lc 'usermod -aG dialout ubuntu'
```

New `wsl` invocation is needed for the group to apply. Verify with `test -r /dev/ttyACM0 -a -w /dev/ttyACM0 && echo RW-OK`.

## 3. Chip communication (verified 2026-09-26)

Toolchain: `uv 0.11.19`, `esptool v5.4.0` via `uvx` (no `pip`/`ensurepip` in this distro image).

```bash
uvx esptool version
timeout 60 uvx esptool --port /dev/ttyACM0 chip_id
timeout 60 uvx esptool --port /dev/ttyACM0 flash_id
```

Notes:
- `chip_id` / `flash_id` print deprecation warnings; canonical names are `chip-id` / `flash-id`.
- Verified result: `Detecting chip type... ESP32-P4`, stub flasher upload/run OK, `Detected flash size: 16MB` (`Manufacturer: 46`, `Device: 4018`).
- Reading chip/flash info does not overwrite firmware. Flash writes are a separate step — decide backup + image first.
