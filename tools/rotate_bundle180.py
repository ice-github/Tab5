#!/usr/bin/env python3
"""Losslessly rotate every JPEG in a Tab5 bundle by 180 degrees.

Usage: rotate_bundle180.py INPUT_DIR OUTPUT_DIR

OUTPUT_DIR must not exist. The original bundle is never modified. JPEGs are
transformed with jpegtran -perfect, and the output index is rebuilt because
lossless transforms may change compressed frame sizes.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def dimensions(jpeg: bytes) -> tuple[int, int]:
    if jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
        raise ValueError("invalid JPEG SOI/EOI")
    pos = 2
    while pos < len(jpeg):
        if jpeg[pos] != 0xFF:
            raise ValueError(f"invalid JPEG marker at byte {pos}")
        marker = jpeg[pos + 1]
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            pos += 2
            continue
        if pos + 4 > len(jpeg):
            break
        seg_len = struct.unpack_from(">H", jpeg, pos + 2)[0]
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6,
                      0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if seg_len < 7:
                break
            height, width = struct.unpack_from(">HH", jpeg, pos + 5)
            return width, height
        if marker == 0xDA:  # start of entropy-coded scan; no SOF found
            break
        pos += 2 + seg_len
    raise ValueError("JPEG has no supported SOF marker")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    if not src.is_dir():
        raise SystemExit(f"input directory does not exist: {src}")
    if dst.exists():
        raise SystemExit(f"output path already exists; refusing to overwrite: {dst}")

    meta = json.loads((src / "meta.json").read_text())
    width, height = int(meta["w"]), int(meta["h"])
    count = int(meta["frames"])
    raw_idx = (src / "frames.idx").read_bytes()
    if len(raw_idx) < 4:
        raise SystemExit("frames.idx is truncated")
    indexed_count = struct.unpack_from("<I", raw_idx)[0]
    if indexed_count != count or len(raw_idx) != 4 + 8 * count:
        raise SystemExit("frame count/index size mismatch")
    entries = [struct.unpack_from("<II", raw_idx, 4 + 8 * i)
               for i in range(count)]
    source_size = (src / "frames.mjpeg").stat().st_size
    expected_off = 0
    for i, (off, length) in enumerate(entries):
        if off != expected_off or length == 0:
            raise SystemExit(f"non-contiguous/empty source index entry {i}")
        expected_off += length
    if expected_off != source_size:
        raise SystemExit("source index does not cover frames.mjpeg exactly")

    # Use an isolated staging directory. Publish the finished bundle only
    # after every frame and sidecar has been written successfully.
    stage = dst.with_name(dst.name + ".partial")
    if stage.exists():
        raise SystemExit(f"staging path already exists; refusing to overwrite: {stage}")
    stage.mkdir(parents=True)
    new_entries: list[tuple[int, int]] = []
    out_off = 0
    try:
        with (src / "frames.mjpeg").open("rb") as inp, \
                (stage / "frames.mjpeg").open("xb") as out:
            for i, (off, length) in enumerate(entries):
                inp.seek(off)
                frame = inp.read(length)
                if len(frame) != length or dimensions(frame) != (width, height):
                    raise ValueError(f"source frame {i + 1} has wrong length/dimensions")
                result = subprocess.run(
                    ["jpegtran", "-rotate", "180", "-perfect", "-copy", "all"],
                    input=frame, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    check=False,
                )
                if result.returncode:
                    raise RuntimeError(
                        f"jpegtran failed on frame {i + 1}: "
                        f"{result.stderr.decode(errors='replace').strip()}"
                    )
                if dimensions(result.stdout) != (width, height):
                    raise ValueError(f"rotated frame {i + 1} dimensions changed")
                new_entries.append((out_off, len(result.stdout)))
                out.write(result.stdout)
                out_off += len(result.stdout)
                if (i + 1) % 300 == 0 or i + 1 == count:
                    print(f"rotated {i + 1}/{count} frames; {out_off} bytes", flush=True)

        with (stage / "frames.idx").open("xb") as f:
            f.write(struct.pack("<I", count))
            for off, length in new_entries:
                f.write(struct.pack("<II", off, length))
        (stage / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
        shutil.copyfile(src / "audio.pcm", stage / "audio.pcm")
        os.rename(stage, dst)
    except Exception:
        print(f"incomplete staging data retained at: {stage}", file=sys.stderr)
        raise
    print(f"created {dst}: {count} losslessly rotated frames, {out_off} bytes")


if __name__ == "__main__":
    main()
