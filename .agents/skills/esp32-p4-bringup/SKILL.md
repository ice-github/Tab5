---
name: esp32-p4-bringup
description: Use when bringing up ESP32-P4 firmware (e.g. M5Stack Tab5), hitting bootloader/chip-revision mismatch, USB-Serial-JTAG console transfer failures, SD/FatFS fopen errors, boot panics or reboot loops, or watchdog resets during bulk I/O. Trigger when flashing fails, the device reboots in a loop, or host-device serial transfer stalls or corrupts data.
---

# ESP32-P4 Bring-up and USB-Serial-JTAG Transfer

Record symptoms first, change second. Every fix below was earned by
observing the failure before editing.

## 1. Chip revision vs ESP-IDF version (check before flashing)

- Read the actual silicon revision: `esptool.py --port <PORT> chip_id`.
  Early Tab5 units report ESP32-P4 **revision v1.0**.
- Newer ESP-IDF bootloaders can refuse old silicon, e.g. IDF v5.5:
  `bootloader.bin requires chip revision in range [v3.1 - v3.99]`.
- Fix: build with an IDF release whose bootloader supports the observed
  revision (v1.0 silicon works with `release/v5.4`). Never `--force` a
  mismatched bootloader onto old silicon.
- Gate: `flash` must succeed without `--force`.

## 2. Panic triage (decode before fixing)

- Capture the full panic: `Guru Meditation` line, `MEPC`, `MCAUSE`, and
  the `Backtrace` addresses.
- Decode program counters, e.g.
  `riscv32-esp-elf-addr2line -e build/<app>.elf -pf <addr>...`
- Common signatures seen on this project:
  - Crash inside `usb_serial_jtag_driver_install` with a NULL config ->
    pass an explicit `usb_serial_jtag_driver_config_t` with nonzero
    `rx_buffer_size`/`tx_buffer_size` (the driver rejects NULL).
  - `Stack protection fault` in `main` -> the default main-task stack is
    too small for multi-KB transfer buffers; raise it
    (`CONFIG_MAIN_TASK_STACK_SIZE=16384`) and move large buffers to
    `static` storage.
  - Silence right after `app_main` entry with no panic -> suspect RX
    starvation or an immediate read-path failure (see section 4).
- Gate: identify the faulting function before editing.

## 3. Reboot-loop triage

- Timestamp `TAB5_XFER_READY`-equivalent markers vs `ESP-ROM` lines. A
  fixed short period (here ~1s, i.e. READY then ROM immediately) means the
  firmware itself reboots right after the handshake, not a host problem.
- Suspect any read call that can return "no data, try later" as an
  immediate failure: one early-`return -1` on an empty read turns into an
  error path that calls `esp_restart()`.
- Unit check that bit this project: `esp_timer_get_time()` returns
  **microseconds**. A `120 * 1000` "millisecond" deadline is 120ms, not
  120s. Write the unit in a comment (`/* 120 s */`) and re-read the
  arithmetic on every deadline.
- Gate: READY must stay up with no host input for longer than one
  deadline period before blaming the host.

## 4. USB-Serial-JTAG RX: use the driver API directly, never VFS fread

- VFS `fread()` semantics flip with driver state: without the driver it
  returns 0 immediately; with the driver installed it blocks forever
  (`portMAX_DELAY`). Neither gives poll-with-deadline behavior, and each
  failure mode is silent (spinning reboot cycle vs. total hang).
- Recipe that works: `usb_serial_jtag_driver_install()` with a large RX
  buffer (16384 here), then `usb_serial_jtag_vfs_use_driver()` so TX
  works, and **RX via `usb_serial_jtag_read_bytes()` with explicit tick
  timeouts** (20ms bulk / 5ms line / 10ms drain polls) inside an
  `esp_timer_get_time()` deadline loop. Write every deadline with its
  unit in a comment — microseconds bit this project once.
- Installing the driver but forgetting `use_driver()` is worse than not
  installing it: the ISR drains the HW FIFO into the ringbuffer while
  VFS polls the now-empty FIFO, so reads return 0 forever, `GO` is never
  sent, and the rx deadline reboots the device every cycle.
- Before the READY marker, drain stale RX bytes left from previous boots
  or old-protocol senders, or the first header/payload is corrupt.
- Skip blank lines when parsing text headers (stale newlines, terminal
  noise), but fail loudly on a wrong filename or malformed header.
- Gate: a probe (`FILE <smallest> ...` then watch `GO`/reboot behavior)
  must behave deterministically before sending megabytes.

## 5. Binary over console: line endings and buffer size

- The VFS layer translates line endings by default and **deterministically
  corrupts binary payloads**. Set both directions to raw LF:
  `usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF)` and the
  TX equivalent.
- The default RX path is a tiny FIFO that drops bytes while the device is
  busy (e.g. slow SD writes). Install the driver with a large RX buffer
  (`rx_buffer_size = 16384` here) for bulk transfer.
- Symptom that points here: small files pass, large files fail CRC with
  the **same wrong value every run** (deterministic mutation, not random
  loss). Confirm by CRC-ing the payload with candidate transforms
  (CR/LF strip, CRLF fold) on the host — or just fix the endings and
  re-test.
- Chunked handshake (`NEXT`/`SEND`/chunk/`ACK` per 4KB) bounds in-flight
  data when the receiver writes slowly.
- Gate: identical CRC mismatch across runs -> check line endings before
  touching chunk size or timing.

## 6. SD / FatFS config: check the generated sdkconfig

- `sdkconfig.defaults` is not the truth; the generated `sdkconfig` is.
  If a defaults entry (e.g. `CONFIG_FATFS_LFN_HEAP=y`) has no effect,
  delete `sdkconfig`/`sdkconfig.old` and rebuild so defaults regenerate.
- Long filenames (`meta.json`, `frames.mjpeg`) need LFN enabled; without
  it `fopen` fails with `errno=22` (EINVAL) even though `stat` on the
  directory succeeds. The BSP logs a warning at boot when LFN is off —
  read it.
- Never enable format-on-mount-fail on a card you do not want wiped
  (`CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL=n`).
- Gate: `grep CONFIG_FATFS_LFN <build>/../sdkconfig` (or the firmware
  `sdkconfig`) shows the intended setting after a clean regenerate.

## 7. Watchdog during bulk transfer

- Tight poll loops and bulk SD writes starve the idle task and trip the
  task watchdog (IDLE0/IDLE1). Distinguish "transfer phase" (not
  real-time; watchdog handling belongs here) from "playback phase"
  (real-time; feed with `esp_task_wdt_reset()` in the loop).
- Do not cargo-cult watchdog changes: confirm the watchdog backtrace
  names the transfer path before adjusting.
- Gate: multi-MB transfer completes with no `task_wdt` lines in the log.

## 8. Host sender discipline

- Log every device line (`[dev] ...`) but match protocol lines by prefix;
  boot logs share the same wire as the protocol.
- `expect()` helper: wait for a line with a wanted prefix, log the rest,
  and abort on too many consecutive empty reads (peer likely dead) rather
  than hanging forever.
- Print progress per N chunks (chunks done, MB, KB/s) so "process alive"
  and "transfer advancing" are distinguishable. A live process with no
  chunk progress is stuck, not slow.
- Verify by re-reading the written file on-device and comparing CRC
  before reporting success.
- Gate: sender output shows monotonic chunk progress; final `OK <CRC>`
  per file plus `COMPLETE`.
