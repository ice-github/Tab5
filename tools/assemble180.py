#!/usr/bin/env python3
"""Assemble media.bundle180/q{q}/ frames.mjpeg + frames.idx + meta.json.

Layout (same as 30s bundle): frames 1-30 and 5371-5400 from the RGB-domain
fade set (true-black endpoints), 31-5370 from the YUV-domain set.
Usage: python3 tools/assemble180.py 3   # or 4
"""
import json
import struct
import sys

Q = sys.argv[1] if len(sys.argv) > 1 else "3"
N = 5400
BASE = "media.bundle180"
YUV = f"{BASE}/jq{Q}"
RGB = f"{BASE}/jq{Q}rgb"
OUT = f"{BASE}/q{Q}"


def pick(i):
    # i: 0-based bundle frame -> source path
    if i < 30 or i >= N - 30:
        return f"{RGB}/f{i + 1:04d}.jpg"
    return f"{YUV}/f{i + 1:04d}.jpg"


def main():
    import os
    os.makedirs(OUT, exist_ok=True)
    offs = []
    off = 0
    with open(f"{OUT}/frames.mjpeg", "wb") as out:
        for i in range(N):
            with open(pick(i), "rb") as f:
                d = f.read()
            assert d[:2] == b"\xff\xd8", pick(i)
            offs.append((off, len(d)))
            out.write(d)
            off += len(d)
    with open(f"{OUT}/frames.idx", "wb") as f:
        f.write(struct.pack("<I", N))
        for o, ln in offs:
            f.write(struct.pack("<II", o, ln))
    meta = {
        "w": 720, "h": 1280, "fps": 30, "frames": N, "pix": "rgb565",
        "audio": {"rate": 48000, "ch": 2, "bits": 16, "file": "audio.pcm"},
        "video": {"file": "frames.mjpeg", "index": "frames.idx"},
    }
    with open(f"{OUT}/meta.json", "w") as f:
        json.dump(meta, f, indent=2)
    import shutil
    shutil.copy(f"{BASE}/audio.pcm", f"{OUT}/audio.pcm")
    print(f"q{Q}: mjpeg={off} bytes frames={N}")


if __name__ == "__main__":
    main()
