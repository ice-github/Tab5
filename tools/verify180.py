#!/usr/bin/env python3
"""Verify a media.bundle180/q{q}/ bundle. Usage: verify180.py [3|4] (in q dir ok)."""
import json
import os
import struct
import subprocess
import sys
import zlib

W, H = 720, 1280
Q = sys.argv[1] if len(sys.argv) > 1 else "3"
BASE = f"media.bundle180/q{Q}"
N = 5400
NSAMP = 8640000  # 48000*180 stereo frames


def sof_dims(d):
    i = 2
    while i < len(d):
        assert d[i] == 0xFF, i
        m = d[i + 1]
        if m in (0xC0, 0xC2):
            h = (d[i + 5] << 8) | d[i + 6]
            w = (d[i + 7] << 8) | d[i + 8]
            return w, h
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        i += 2 + ((d[i + 2] << 8) | d[i + 3])
    raise ValueError("no SOF")


def getframe(ents, i):
    o, ln = ents[i]
    with open(f"{BASE}/frames.mjpeg", "rb") as f:
        f.seek(o)
        return f.read(ln)


def decluma(d):
    import numpy as np
    p = subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "mjpeg", "-i", "pipe:0",
         "-vf", "scale=in_color_matrix=bt709:out_color_matrix=bt709",
         "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
        input=d, capture_output=True)
    a = np.frombuffer(p.stdout, np.uint8).reshape(H, W, 3).astype(float)
    return 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]


meta = json.load(open(f"{BASE}/meta.json"))
print("meta:", meta)
assert (meta["w"], meta["h"], meta["fps"], meta["frames"]) == (720, 1280, 30, N)
raw = open(f"{BASE}/frames.idx", "rb").read()
n = struct.unpack("<I", raw[:4])[0]
print("idx nframes=", n, "idx size=", len(raw))
assert n == N == meta["frames"] and len(raw) == 4 + 8 * N
ents = [struct.unpack("<II", raw[4 + 8 * i:12 + 8 * i]) for i in range(n)]
mj = os.path.getsize(f"{BASE}/frames.mjpeg")
assert ents[0][0] == 0
for i, (o, ln) in enumerate(ents):
    eo, el = ents[i + 1] if i + 1 < n else (mj, 0)
    assert o + ln == eo, i
assert ents[-1][0] + ents[-1][1] == mj
print("idx chain OK, mjpeg size=", mj)
means = {}
for i in (0, 1, 14, 28, 29, 30, 31, 2699, 2700, 5368, 5369, 5370, 5371,
          5398, 5399):
    d = getframe(ents, i)
    w, h = sof_dims(d)
    m = decluma(d).mean()
    means[i] = m
    print(f"frame {i + 1}: SOI={d[:2] == bytes([255, 216])} "
          f"dims={w}x{h} meanY={m:.1f}")
    assert (w, h) == (720, 1280)
Y0m = decluma(getframe(ents, 0)).mean()
YLm = decluma(getframe(ents, N - 1)).mean()
Y0x = decluma(getframe(ents, 0)).max()
YLx = decluma(getframe(ents, N - 1)).max()
print(f"endpoints: f1 mean={Y0m:.1f} max={Y0x:.0f} | "
      f"f{N} mean={YLm:.1f} max={YLx:.0f}")
assert Y0m < 8 and YLm < 8 and Y0x <= 16 and YLx <= 16, "endpoints not black"
assert decluma(getframe(ents, 2700)).mean() > 40, "middle dark"
head = [means[i] for i in (0, 1, 14, 28, 29, 30, 31)]
tail = [means[i] for i in (5368, 5369, 5370, 5371, 5398, 5399)]
assert all(b >= a - 3 for a, b in zip(head, head[1:])), head
assert all(b <= a + 3 for a, b in zip(tail, tail[1:])), tail
print("fade ramps OK")
import numpy as np
a = np.fromfile(f"{BASE}/audio.pcm", dtype=np.int16).reshape(-1, 2)
print("audio samples=", len(a), "bytes=", len(a) * 4)
assert len(a) == NSAMP and os.path.getsize(f"{BASE}/audio.pcm") == NSAMP * 4
for tag, s in [("start100ms", a[:4800]), ("end100ms", a[-4800:]),
               ("mid100ms", a[4320000:4324800])]:
    print(f"audio {tag}: maxabs={np.abs(s).max()} "
          f"meanabs={np.abs(s).astype(float).mean():.1f}")
assert np.abs(a[:4800]).max() < 200 and np.abs(a[-4800:]).max() < 200
assert np.abs(a[48000:-48000]).max() > 1000, "middle quiet"
print("audio OK")
for f in ("meta.json", "frames.mjpeg", "frames.idx", "audio.pcm"):
    p = f"{BASE}/{f}"
    print(f"{f}: size={os.path.getsize(p)} "
          f"crc32={zlib.crc32(open(p, 'rb').read()):08x}")
print("ALL CHECKS PASSED")
