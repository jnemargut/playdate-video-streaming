# The `.rwlpv` stream format

`.rwlpv` ("Read Watch Listen Play Video") is a deliberately dumb container built for
**progressive streaming**: a tiny fixed header followed by a sequence of
**self-delimiting packets**. There is no central index and no seek table — a decoder
can start playing as soon as it has the header plus a few packets, which is the whole
point.

All multi-byte integers are **little-endian**.

## Header — 16 bytes

| Offset | Size | Field | Notes |
|--:|--:|---|---|
| 0  | 8 | magic | ASCII `"RWLPVID1"` |
| 8  | 2 | width  | `u16` — pixels (engine expects **400**) |
| 10 | 2 | height | `u16` — pixels (engine expects **240**) |
| 12 | 2 | fps    | `u16` — video frame rate (typically **6**) |
| 14 | 2 | sampleRate | `u16` — informational; audio is decoded to 44.1 kHz |

The engine tolerates a missing/garbled magic (it falls through to reading packets),
but always write it.

## Packets

Immediately after the header, packets repeat until end-of-stream:

| Size | Field | Notes |
|--:|---|---|
| 1 | type | `u8` — `1` = video, `2` = audio |
| 4 | len  | `u32` — payload length in bytes |
| len | payload | see below |

### Video packet (`type = 1`)

A **raw 1-bit frame bitmap**: `width * height / 8` bytes (12,000 for 400×240). One bit
per pixel, MSB-first, row-major — the same layout Playdate's `playdate->graphics->getBitmapData`
exposes, so the engine blits it directly into an `LCDBitmap`.

### Audio packet (`type = 2`)

A chunk of a **constant-bitrate MP3** elementary stream (mono, 64 kbps in the reference
encoder). Chunks are just slices of the MP3 byte stream; the decoder (minimp3) finds
frame boundaries itself, so a packet need not align to an MP3 frame.

## Interleaving (why it streams smoothly)

The encoder emits, for each video frame, **all the audio up to that frame's timestamp
first, then the video frame**. Audio therefore always arrives slightly ahead of the
picture it accompanies. Because the engine drives video off the audio sample clock,
this keeps A/V in sync and means a frame is in hand by the time its moment arrives —
even mid-stream, with no buffering of the whole file.

## Minimal pseudo-encoder

```
write "RWLPVID1", u16 width, u16 height, u16 fps, u16 44100
for each frame i at time t = i / fps:
    while audio_written_seconds < t:
        blk = next ~chunk of the CBR-MP3 stream
        write u8 2, u32 len(blk), blk
    frame = 1bpp_bitmap(dither(scaled(source_frame_i)))   # 12000 bytes
    write u8 1, u32 len(frame), frame
flush any remaining audio as type-2 packets
```

See [`tools/encode-stream-video.py`](../tools/encode-stream-video.py) for the real thing.
