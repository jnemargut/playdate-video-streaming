# Playdate Video Streaming

**Real-time 1-bit video + audio streaming for the [Playdate](https://play.date), in pure C + Lua.**

The Playdate's built-in video format (`.pdv`) has to be **fully present on disk** before it plays — there's no native way to play a film *as it downloads*. This is a small native engine that fixes that: it streams a custom `.rwlpv` file over HTTP(S), decoding and playing the picture and sound **as the bytes arrive**.

It was built for the daily-gift app *Read Watch Listen Play*, and pulled out here so anyone can use it. **Contributions very welcome** — see [Contributing](#contributing).

> 🔊 **Streaming audio too?** There's a companion engine with the same design:
> **[playdate-audio-streaming](https://github.com/jnemargut/playdate-audio-streaming)** (on-demand
> MP3 over HTTP, plus live internet radio). See [Using both together](#using-both-together) — a
> `.pdx` may export only one `eventHandler`.

---

## What you get

- **Stream while downloading** — playback starts after a ~3s prebuffer, not after the whole file.
- **Synced audio** — the video is locked to the audio clock, so picture and sound stay together.
- **Flow-controlled** — reads are gated on engine room (`roomC`), so a fast network can't overrun the buffers (and memory stays bounded no matter how long the clip is).
- **Stream + Save** — optionally write the stream to disk as it plays, then replay it offline from a file.
- **Tiny surface** — one image to draw, a `tick()` to call, and you're done.

## How it works

1. You pre-encode your source video into a **`.rwlpv`** file (see [Encoding](#encoding)): a 16-byte header followed by interleaved packets of **raw 1-bit video frames** and **CBR-MP3 audio**, ordered so the audio for a moment arrives just before the frame it belongs to. Full spec in [`docs/FORMAT.md`](docs/FORMAT.md).
2. At runtime, Lua does the HTTP GET and feeds the incoming bytes to the native engine **in order**, only while it has room.
3. The C engine ([`src/streamvideo.c`](src/streamvideo.c)) decodes the MP3 (via the vendored [minimp3](https://github.com/lieff/minimp3)), resamples to 44.1 kHz, plays it through a sound source, and — using the number of audio samples played as a clock — blits the right 1-bit frame into an image you draw each update.

Everything is 400×240, 1-bit, and designed to be gentle on the device's memory and single network connection.

## Quick start

### 1. Add the engine to your build

Copy `src/streamvideo.c`, `src/streamvideo_entry.c`, `src/minimp3.h`, and `src/minimp3_impl.c` into your project and add the `.c` files to your `SRC`:

```make
HEAP_SIZE  = 8388208
STACK_SIZE = 61800
SRC = src/streamvideo.c src/streamvideo_entry.c src/minimp3_impl.c
include $(SDK)/C_API/buildsupport/common.mk
```

> Already have a native `eventHandler`? **Don't** compile `streamvideo_entry.c`. Instead, from your own handler call `streamvideo_setPD(pd)` on `kEventInit` and `eventHandler_streamvideo(pd, event, arg)` on every event. (Two `extern` declarations; see the entry file.)

### 2. Drop in the Lua front-end

Copy `lua/StreamVideo.lua` into your source and `import` it.

```lua
import "CoreLibs/graphics"
import "StreamVideo"

local gfx = playdate.graphics

StreamVideo.configure("your-host.example.com", 443, true)  -- host, port, useSSL

function playdate.update()
    if not StreamVideo.isActive() then
        StreamVideo.play("clips/demo.rwlpv")    -- streams https://your-host/clips/demo.rwlpv
    end
    StreamVideo.tick()                          -- pump bytes + advance a frame

    local img = StreamVideo.image()
    if StreamVideo.isReady() and img then
        img:draw(0, 0)
    else
        gfx.drawTextAligned("Buffering…", 200, 116, kTextAlignment.center)
    end
end
```

A slightly fuller example is in [`example/main.lua`](example/main.lua).

### 3. Make a `.rwlpv`

```sh
python3 tools/encode-stream-video.py input.mp4 --out demo.rwlpv --fps 6 --secs 60 --title "Demo"
```

Requires `ffmpeg` on your PATH. Host the resulting file wherever your app fetches from (Cloudflare R2, S3, your own server — anything that serves it over HTTP(S)).

## Lua API

| Call | Purpose |
|---|---|
| `StreamVideo.configure(host, port, ssl)` | Where to stream from. Call once. |
| `StreamVideo.available()` | `true` if the native engine is present and complete. |
| `StreamVideo.setImage(img)` / `image()` | Provide / get the image frames are blitted into (a 400×240 one is made if you don't). |
| `StreamVideo.play(path[, cachePath[, onCached]])` | Stream `/path`. With `cachePath`, also save to disk; `onCached(path)` fires when the save finishes. |
| `StreamVideo.playFile(localPath)` | Replay a cached `.rwlpv` from disk (no network). |
| `StreamVideo.tick()` | Call every `playdate.update()`. |
| `StreamVideo.detach()` | Leave the player but finish writing the cache in the background. |
| `StreamVideo.stop()` | Tear everything down (crash-safe). |
| `isReady()` / `isPlaying()` / `isFinished()` / `isActive()` / `isCaching()` | State. |
| `posSeconds()` / `progress()` | Playback position; `(bytesDown, bytesTotal)`. |

The native functions (`streamvideo.startC`, `feedC`, `roomC`, `tickC`, `finalizeC`, `isReadyC`, …) are registered directly on the global `streamvideo` table if you'd rather drive the engine yourself.

## Encoding

`tools/encode-stream-video.py` wraps `ffmpeg`:

```
python3 tools/encode-stream-video.py <input|url> --out clip.rwlpv [--fps 6] [--secs 0] [--contrast 1.2] [--title "..."]
```

- `--fps` — frame rate (default **6**; see caveats — keep it low).
- `--secs` — length to encode (`0` = whole file).
- It dithers the video to 1-bit (Floyd–Steinberg), encodes a synced **mono 64 kbps CBR** MP3, and interleaves them into the stream.

## Using both together

This engine ships `streamvideo_entry.c`, which defines the `eventHandler` the runtime
needs. The companion [audio engine](https://github.com/jnemargut/playdate-audio-streaming)
defines its own `eventHandler` too — but a `.pdx` may export only **one**. To use both,
make the **audio** engine's handler the single entry point and add this engine's two hooks
to it (the audio repo marks the exact spots with `// + video engine:` comments), then **do
not** compile `streamvideo_entry.c`. The engines are otherwise independent.

## Caveats & limits

Read these before you build a UI around it:

- **1-bit, 400×240 only.** Frames are raw 1-bit bitmaps. No color, no scaling in the format (pre-scale in the encoder).
- **Keep the frame rate low.** ~**6 fps** is the sweet spot. Each frame is 12,000 bytes of raw video; higher rates blow up the bitrate and the buffers. This is "moving pictures," not 30 fps.
- **Audio must be CBR MP3.** The clock and pacing assume constant bitrate. VBR will drift. The encoder handles this for you; if you roll your own, force CBR.
- **One connection at a time on hardware.** The device is unhappy with concurrent HTTP connections. If you also stream audio elsewhere, or run a background sync, make sure only one connection is live when you stream.
- **TLS comes from `playdate.network.http`.** The Lua HTTP API does the HTTPS; the engine just consumes bytes. (You can feed it from `playdate.network.tcp`, a file, or anywhere else instead.)
- **~3s prebuffer.** Playback waits for `PREBUFFER_BYTES` before starting, so there's a short buffering beat up front.
- **Memory.** A 128 KB audio ring + 32 frame slots (~384 KB) live in the engine; budget `HEAP_SIZE` accordingly.
- **Tested on Playdate hardware (rev A) + Simulator.** Different content and networks will find edges — please report them.

## Contributing

This exists because streaming video on the Playdate *shouldn't* require everyone to reinvent it. If you make it better, send it back so the next person benefits. Issues and PRs are genuinely welcome — including rough ones.

Ideas that would help a lot:

- **Seeking / scrubbing** (the format is currently forward-only).
- **A keyframe/index** so you can start mid-file.
- **Adaptive frame rate or bitrate** for flaky networks.
- **Better dithering options** in the encoder (Atkinson, ordered, contrast curves).
- **A `playdate.network.tcp` transport** (no Lua-side HTTP).
- **Hardware testing** on more units and networks, and bug reports with `.rwlpv` samples.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the (short) details.

## Credits & license

- Built by **[jnemargut](https://github.com/jnemargut)** for *Read Watch Listen Play*.
- MP3 decoding by [minimp3](https://github.com/lieff/minimp3) (CC0 / public domain), vendored in `src/`.
- This project is **MIT licensed** — see [`LICENSE`](LICENSE). Use it, ship it, sell your game with it; just keep the notice.
