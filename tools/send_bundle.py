#!/usr/bin/env python3
"""Send media/bundle/* to Tab5 over USB-serial (/dev/ttyACM0)."""
import binascii
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
FILES = ["meta.json", "frames.mjpeg", "frames.idx", "audio.pcm"]
BUNDLE = "media/bundle"


def readline(ser, timeout=15):
    ser.timeout = timeout
    line = ser.readline().decode("ascii", "replace").strip()
    return line


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
        ser.write(f"FILE {name} {len(data)} {crc:08X}\n".encode())
        line = readline(ser)
        assert line == "GO", f"expected GO, got {line!r}"
        ser.write(data)
        print(f"sent {name}, wait OK...", flush=True)
        line = readline(ser, timeout=60)
        assert line == f"OK {crc:08X}", f"mismatch: {line!r}"
        print(f"OK {name}", flush=True)

    ser.write(b"DONE\n")
    line = readline(ser, timeout=15)
    assert line == "COMPLETE", f"expected COMPLETE, got {line!r}"
    print("transfer complete", flush=True)


if __name__ == "__main__":
    main()
