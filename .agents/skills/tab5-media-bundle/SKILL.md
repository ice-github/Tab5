---
name: tab5-media-bundle
description: Use when preparing a portrait video bundle for the Tab5 player: downloading a YouTube clip, cutting the first 10 seconds, rotating to 720x1280 JPEG frames, building frames.mjpeg + frames.idx, resampling audio to 48kHz stereo PCM, and verifying the bundle. Trigger when the user wants a new clip on the device or the player reports frame/audio mismatch.
---

# Tab5 Portrait Media Bundle (10s, 720x1280 JPEG + PCM)

Output is `media/bundle/` (git-ignored, regenerable): `meta.json`,
`frames.mjpeg`, `frames.idx`, `audio.pcm`. Keep `media/` out of git;
only the recipe and verification live in the repo.

## 1. Source acquisition

- `yt-dlp` with explicit format selection. For the reference clip
  (`https://www.youtube.com/watch?v=ilqlSBYfCGs`) video format `136`
  (720p H.264) + audio format `140` (AAC) were used.
- For a new URL, list formats first and pick a 720p H.264 video stream
  plus the best audio stream; do not assume format codes carry over.
- Cut the first 10s into a working file (e.g. `media/src10s.mp4`,
  1280x720 landscape, AAC 44.1kHz stereo in the reference case).

## 2. Video: portrait JPEG frames

- Target: 300 frames, 30fps, each 720 wide x 1280 high, pre-rotated on
  the PC so the device does no rotation.
- Reference properties (verified): every frame starts with SOI
  (`FF D8`), SOF0/SOF2 reports 720x1280.
- Concatenate the JPEGs in order into `frames.mjpeg` (~7.2MB for the
  reference clip).

## 3. Index: frames.idx layout (little-endian)

- `u32 nframes`, then `nframes` entries of `(u32 offset, u32 length)`.
- Total size must equal `4 + 8 * nframes` (2404 bytes for 300 frames).
- Verify: first entry offset is 0, each `offset+length` lands on the next
  frame boundary, and the last frame ends exactly at EOF of
  `frames.mjpeg`.

## 4. Audio: audio.pcm layout

- Raw signed 16-bit little-endian stereo at 48000Hz, no header
  (~1.92MB for 10s: `48000 * 2ch * 2B * 10s = 1,920,000` bytes; small
  padding from resampling is acceptable but must be explainable).
- Resample from the source rate (44.1kHz in the reference) to 48kHz on
  the PC; the player assumes `A_RATE=48000`, `A_CH=2`.

## 5. Manifest: meta.json

```json
{
 "w": 720, "h": 1280, "fps": 30, "frames": 300, "pix": "rgb565",
 "audio": {"rate": 48000, "ch": 2, "bits": 16, "file": "audio.pcm"},
 "video": {"file": "frames.mjpeg", "index": "frames.idx"}
}
```

- `w/h/fps/frames` must match the actual JPEGs and index count; the
  player trusts these values.

## 6. Verification (run before any device transfer)

1. `meta.json` parses and `frames` equals the index count.
2. `frames.idx` size is `4 + 8*nframes`; spot-check frames (first,
   middle, last) for SOI magic and 720x1280 dimensions.
3. `audio.pcm` size is consistent with rate/ch/bits/duration.
4. CRC32 each file; the sender and device compare these after transfer.

## 7. Rules

- Conversion itself belongs in a re-runnable script; this skill holds the
  format contract and the checks, not one-off shell history.
- Never commit `media/` output; never transfer an unverified bundle.
  A bundle that fails check 2 or 3 will fail on-device CRC or playback
  — fix it on the PC, not on the device.
