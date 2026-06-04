#!/usr/bin/env python3
"""
encode-stream-video.py — turn a video into a streamable .rwlpv file for the
Watch pillar's real-time streaming.

Unlike .pdv (which needs the whole file + a frame table before frame one), .rwlpv
is a *stream*: a tiny header followed by interleaved, self-delimiting packets of
raw 1-bit video frames and CBR-MP3 audio. The C engine reads packets as they
arrive over TCP/HTTP, decodes audio with minimp3, and shows each frame synced to
the audio clock — so playback starts after a short prebuffer.

Bandwidth math (must stay under the Playdate's ~100 KB/s HTTPS):
  video: raw 1-bit 400x240 = 12000 B/frame; at 6 fps = 72 KB/s
  audio: CBR 64 kbps mono   = 8 KB/s
  total ~80 KB/s  -> streams in real time with headroom.

Format:
  header (16 B): "RWLPVID1"(8) | width u16 | height u16 | fps u16 | sampleRate u16   (all LE)
  packets:       type u8 (1=video, 2=audio) | len u32 LE | payload[len]
  video payload: 12000 bytes (PIL '1' tobytes: 50 B/row * 240, MSB first, 1=white)
  audio payload: a slice of the CBR MP3 byte stream

  python3 pipeline/encode-stream-video.py <url|file> --out dist/2026-06-03/watch.rwlpv \\
          --fps 6 --title "..."   [--secs N]
"""
import sys, os, subprocess, tempfile, struct, glob, argparse, json
from PIL import Image

W, H = 400, 240
AUDIO_KBPS = 64
AUDIO_BYTES_PER_SEC = AUDIO_KBPS * 1000 // 8   # CBR -> bytes are ~proportional to time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out", required=True)
    ap.add_argument("--fps", type=int, default=6)
    ap.add_argument("--secs", type=int, default=0)   # 0 = full length
    ap.add_argument("--contrast", type=float, default=1.2)
    ap.add_argument("--title", default="Today's Short")
    a = ap.parse_args()
    tflag = (["-t", str(a.secs)] if a.secs and a.secs > 0 else [])

    with tempfile.TemporaryDirectory() as tmp:
        # 1) frames -> full-res 1-bit (Floyd-Steinberg, the crisp look)
        vf = (f"fps={a.fps},scale={W}:{H}:force_original_aspect_ratio=increase,"
              f"crop={W}:{H},format=gray,eq=contrast={a.contrast}")
        subprocess.run(["ffmpeg", "-y", "-v", "error", *tflag, "-i", a.input,
                        "-vf", vf, os.path.join(tmp, "f_%06d.png")], check=True)
        frames = sorted(glob.glob(os.path.join(tmp, "f_*.png")))
        if not frames:
            sys.exit("ffmpeg produced no frames")

        # 2) audio -> CBR 64k mono mp3 (so byte offset ~ time offset)
        mp3 = os.path.join(tmp, "a.mp3")
        r = subprocess.run(["ffmpeg", "-y", "-v", "error", *tflag, "-i", a.input,
                            "-vn", "-ar", "44100", "-ac", "1", "-b:a", f"{AUDIO_KBPS}k",
                            "-write_xing", "0", mp3])
        audio = b""
        if r.returncode == 0 and os.path.exists(mp3) and os.path.getsize(mp3) > 2048:
            with open(mp3, "rb") as f:
                audio = f.read()

        # 3) interleave: header, then for each frame emit the audio up to its time
        #    followed by the frame packet (audio leads so frames arrive on time).
        n = len(frames)
        with open(a.out, "wb") as out:
            out.write(b"RWLPVID1" + struct.pack("<HHHH", W, H, a.fps, 44100))
            apos = 0
            for i, fp in enumerate(frames):
                t = i / a.fps
                atarget = min(len(audio), int(AUDIO_BYTES_PER_SEC * t))
                if atarget > apos:
                    blk = audio[apos:atarget]
                    out.write(struct.pack("<BI", 2, len(blk))); out.write(blk)
                    apos = atarget
                img = Image.open(fp).convert("1")
                if img.size != (W, H):
                    img = img.resize((W, H))
                data = img.tobytes()  # 12000 bytes
                out.write(struct.pack("<BI", 1, len(data))); out.write(data)
            if apos < len(audio):
                blk = audio[apos:]
                out.write(struct.pack("<BI", 2, len(blk))); out.write(blk)

        secs = round(n / a.fps)
        # sidecar meta (title + seconds + hasAudio) reused by the app
        json.dump({"title": a.title, "seconds": secs, "fallback": False, "stream": True},
                  open(os.path.splitext(a.out)[0] + ".json", "w"), indent=2)
        mb = os.path.getsize(a.out) / 1e6
        print(f"wrote {a.out}  ({n} frames @ {a.fps}fps, {W}x{H}, ~{secs}s, {mb:.1f}MB, "
              f"{'audio' if audio else 'silent'})")

if __name__ == "__main__":
    main()
