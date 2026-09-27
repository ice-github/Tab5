#!/usr/bin/env python3
"""Send media/bundle/* to Tab5 over USB-serial (/dev/ttyACM0)."""
import binascii
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
FILES = ["meta.json", "frames.mjpeg", "frames.idx", "audio.pcm"]
BUNDLE = sys.argv[2] if len(sys.argv) > 2 else "media/bundle"


def readline(ser, timeout=15):
    ser.timeout = timeout
    line = ser.readline().decode("ascii", "replace").strip()
    return line


def expect(ser, prefixes, timeout=60, idle_limit=12, deadline=None, fail_on=()):
    """Read lines until one starts with a wanted prefix; log others.

    Raises on too many consecutive empty reads (peer likely dead),
    on passing deadline, or on fail_on markers (e.g. device rebooted
    and re-sent READY mid-transfer)."""
    if isinstance(prefixes, str):
        prefixes = (prefixes,)
    idle = 0
    while True:
        if deadline is not None and time.time() > deadline:
            raise SystemExit(f"timeout waiting for {prefixes}")
        line = readline(ser, timeout=timeout)
        if line.startswith(prefixes):
            return line
        if line.startswith(fail_on):
            raise SystemExit(
                f"device reset mid-transfer: got {line!r} "
                f"while waiting for {prefixes}")
        if line:
            print(f"[dev] {line}", flush=True)
            idle = 0
        else:
            idle += 1
            if idle >= idle_limit:
                raise SystemExit(f"stall: no protocol line, last={prefixes}")


def main():
    blobs = {}
    for name in FILES:
        with open(f"{BUNDLE}/{name}", "rb") as f:
            blobs[name] = f.read()
        print(f"{name}: {len(blobs[name])} bytes crc={binascii.crc32(blobs[name]):08X}",
              flush=True)

    ser = serial.Serial(PORT, 115200, timeout=15)
    time.sleep(0.5)
    print("wait TAB5_XFER_READY...", flush=True)
    for _ in range(600):
        line = readline(ser, timeout=5)
        if line == "TAB5_XFER_READY":
            break
        if line:
            print(f"[dev] {line}", flush=True)
    else:
        raise SystemExit("no TAB5_XFER_READY from device")

    for name in FILES:
        data = blobs[name]
        crc = binascii.crc32(data)
        # Any READY/ROM line here means the device rebooted mid-transfer.
        reboot = ("TAB5_XFER_READY", "ESP-ROM:")
        print(f"FILE {name} {len(data)} {crc:08X}", flush=True)
        ser.write(f"FILE {name} {len(data)} {crc:08X}\n".encode())
        line = expect(ser, "GO", deadline=time.time() + 120,
                      fail_on=reboot)
        assert line == "GO", f"expected GO, got {line!r}"
        off = 0
        nchunk = (len(data) + 4095) // 4096
        t0 = time.time()
        i = 0
        while off < len(data):
            chunk = data[off:off + 4096]
            ser.write(b"NEXT\n")
            line = expect(ser, "SEND", deadline=time.time() + 120,
                          fail_on=reboot)
            assert line == "SEND", f"expected SEND, got {line!r}"
            ser.write(chunk)
            line = expect(ser, "ACK", deadline=time.time() + 120,
                          fail_on=reboot)
            assert line == "ACK", f"expected ACK, got {line!r}"
            off += len(chunk)
            i += 1
            if i % 200 == 0 or off >= len(data):
                el = time.time() - t0
                print(f"  {name}: {i}/{nchunk} chunks, "
                      f"{off / 1048576:.1f}MB, {off / max(el, 0.1) / 1024:.0f}KB/s",
                      flush=True)
        print(f"sent {name}, wait OK...", flush=True)
        line = expect(ser, ("OK ", "ERR"), deadline=time.time() + 180,
                      fail_on=reboot)
        assert line == f"OK {crc:08X}", f"mismatch: {line!r}"
        print(f"OK {name}", flush=True)

    ser.write(b"DONE\n")
    line = expect(ser, ("COMPLETE", "ERR"), timeout=15,
                  deadline=time.time() + 120,
                  fail_on=("TAB5_XFER_READY", "ESP-ROM:"))
    assert line == "COMPLETE", f"expected COMPLETE, got {line!r}"
    print("transfer complete", flush=True)


if __name__ == "__main__":
    main()
