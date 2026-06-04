// streamvideo.c — real-time 1-bit video+audio streaming for the Watch pillar.
//
// Plays the .rwlpv stream format (see pipeline/encode-stream-video.py): a 16-byte
// header then interleaved, self-delimiting packets — type 1 = raw 1-bit video frame
// (12000 B), type 2 = CBR-MP3 audio. Lua feeds the bytes (from HTTPS or a cached
// file; the SDK handles TLS), C decodes audio with minimp3 and shows each frame in
// sync with the audio clock by writing into a Lua-owned image. No TCP here.
//
// Pipeline (mirrors streamaudio.c for audio, plus a video frame ring):
//   feed -> parser -> { audio bytes -> ring -> minimp3 -> PCM ring -> audio source }
//                     { video frame -> frame ring -> blit on tick, synced to audio }
//
// Lua surface (registered as streamvideo.*):
//   startC()            begin a fresh stream
//   setTargetC(image)   the playdate.graphics.image to draw frames into (400x240)
//   feedC(bytes) -> n   push stream bytes; returns bytes accepted (rest re-fed later)
//   tickC()             once per frame: decode audio + display the due frame
//   stopC()             tear down
//   isPlayingC() -> b   audio is flowing
//   isReadyC()   -> b   first frame shown / playback started
//   posSecondsC()-> n   playback position (for the progress bar)
//   bytesTextC() -> s   loading meter

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "pd_api.h"
#include "minimp3.h"

#define VW 400
#define VH 240
#define FRAME_BYTES (VW * VH / 8)        // 12000

#define RING_SIZE 131072                 // audio byte ring (128 KB)
#define RING_MASK (RING_SIZE - 1)
#define MP3_SCRATCH 4096
#define PCM_FRAMES 16384
#define PCM_MASK   (PCM_FRAMES - 1)
#define OUTPUT_HZ 44100
#define MAX_RESAMPLED_PER_FRAME 8192
#define FRAME_SLOTS 32                   // ~5s at 6fps
#define PREBUFFER_BYTES 24576            // ~3s of 64k audio before play

static PlaydateAPI* pd  = NULL;

// ---- audio byte ring (producer: feed; consumer: decode) ----
static uint8_t     ring[RING_SIZE];
static atomic_uint ring_write = 0, ring_read = 0;
// ---- mono PCM ring (producer: decode; consumer: audio thread) ----
static int16_t     pcm[PCM_FRAMES];
static atomic_uint pcm_write = 0, pcm_read = 0;
static atomic_uint samples_played = 0;   // total samples output -> the audio clock

static mp3dec_t mp3dec;
static uint8_t  mp3_scratch[MP3_SCRATCH];
static int16_t  decode_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
static SoundSource* vsource = NULL;

// linear-interp resampler (Q16.16)
static int rs_in_rate = 0; static int32_t rs_step_q16 = 0, rs_pos_q16 = 0; static int16_t rs_prev = 0;

// ---- video frame ring ----
static uint8_t  frames[FRAME_SLOTS][FRAME_BYTES];
static int      frame_idx[FRAME_SLOTS];
static unsigned vf_write = 0, vf_read = 0;     // not atomic: both touched on main thread
static LCDBitmap* target = NULL;

// ---- stream header / parser ----
static int      have_header = 0;
static int      v_fps = 6;
static int      started = 0;       // audio source added
static int      ready   = 0;       // first frame shown
static unsigned bytes_total = 0;
static int      last_shown = -1;
static int      net_done   = 0;    // Lua said the feed is complete (stream closed / file EOF)
static int      v_completed = 0;   // the whole film played out to the end

// parser state
enum { PS_HEADER, PS_PKTHEAD, PS_AUDIO, PS_VIDEO };
static int      ps = PS_HEADER;
static uint8_t  hbuf[16];
static unsigned hpos = 0;
static uint32_t body_remaining = 0;
static unsigned vfill = 0;          // bytes written into the current video slot

// ---- ring helpers ----
static inline unsigned ring_used(void) {
	unsigned w = atomic_load_explicit(&ring_write, memory_order_acquire);
	unsigned r = atomic_load_explicit(&ring_read,  memory_order_relaxed);
	return (w - r) & RING_MASK;
}
static inline unsigned ring_free(void) { return RING_SIZE - 1 - ring_used(); }
static unsigned ring_push(const uint8_t* src, unsigned n) {
	unsigned w = atomic_load_explicit(&ring_write, memory_order_relaxed);
	unsigned take = ring_free(); if (n < take) take = n;
	for (unsigned i = 0; i < take; i++) ring[(w + i) & RING_MASK] = src[i];
	atomic_store_explicit(&ring_write, (w + take) & RING_MASK, memory_order_release);
	return take;
}
static unsigned ring_peek_contig(uint8_t* dst, unsigned cap) {
	unsigned r = atomic_load_explicit(&ring_read,  memory_order_relaxed);
	unsigned used = ring_used(); if (used > cap) used = cap;
	for (unsigned i = 0; i < used; i++) dst[i] = ring[(r + i) & RING_MASK];
	return used;
}
static void ring_consume(unsigned n) {
	unsigned r = atomic_load_explicit(&ring_read, memory_order_relaxed);
	atomic_store_explicit(&ring_read, (r + n) & RING_MASK, memory_order_release);
}
static inline unsigned pcm_free(void) {
	unsigned w = atomic_load_explicit(&pcm_write, memory_order_relaxed);
	unsigned r = atomic_load_explicit(&pcm_read,  memory_order_acquire);
	return (PCM_FRAMES - 1) - ((w - r) & PCM_MASK);
}

// ---- decode (main thread) ----
static void pcm_write_frame(const int16_t* src, int samples, int channels, int hz) {
	if (hz != rs_in_rate) { rs_in_rate = hz; rs_step_q16 = (int32_t)(((int64_t)hz << 16) / OUTPUT_HZ); }
	static int16_t mono[MINIMP3_MAX_SAMPLES_PER_FRAME / 2];
	if (channels == 2) for (int i = 0; i < samples; i++) mono[i] = (int16_t)(((int32_t)src[i*2] + src[i*2+1]) >> 1);
	else               for (int i = 0; i < samples; i++) mono[i] = src[i];
	int32_t pos = rs_pos_q16, step = rs_step_q16, end = (int32_t)samples << 16;
	unsigned w = atomic_load_explicit(&pcm_write, memory_order_relaxed);
	int written = 0;
	while (pos < end) {
		int idx = pos >> 16, frac = pos & 0xFFFF;
		int16_t a = (idx == 0) ? rs_prev : mono[idx - 1], b = mono[idx];
		pcm[(w + written) & PCM_MASK] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * frac) >> 16));
		written++; pos += step;
	}
	atomic_store_explicit(&pcm_write, (w + written) & PCM_MASK, memory_order_release);
	rs_prev = mono[samples - 1]; rs_pos_q16 = pos - end;
}
static void decodeIntoPCM(int max_frames) {
	for (int n = 0; n < max_frames; n++) {
		if (pcm_free() < MAX_RESAMPLED_PER_FRAME) break;
		unsigned got = ring_peek_contig(mp3_scratch, MP3_SCRATCH);
		if (got < 4) break;
		mp3dec_frame_info_t info = {0};
		int samples = mp3dec_decode_frame(&mp3dec, mp3_scratch, (int)got, decode_buf, &info);
		if (info.frame_bytes > 0) ring_consume((unsigned)info.frame_bytes); else break;
		if (samples > 0) pcm_write_frame(decode_buf, samples, info.channels, info.hz);
	}
}

// ---- audio callback (audio thread) ----
static int audioCallback(void* ctx, int16_t* left, int16_t* right, int len) {
	(void)ctx; (void)right;
	unsigned r = atomic_load_explicit(&pcm_read,  memory_order_relaxed);
	unsigned w = atomic_load_explicit(&pcm_write, memory_order_acquire);
	unsigned avail = (w - r) & PCM_MASK;
	unsigned want = (unsigned)len < avail ? (unsigned)len : avail;
	unsigned first = PCM_FRAMES - r; if (first > want) first = want;
	memcpy(left, pcm + r, first * sizeof(int16_t));
	if (first < want) memcpy(left + first, pcm, (want - first) * sizeof(int16_t));
	for (unsigned i = want; i < (unsigned)len; i++) left[i] = 0;
	atomic_store_explicit(&pcm_read, (r + want) & PCM_MASK, memory_order_release);
	atomic_fetch_add_explicit(&samples_played, want, memory_order_relaxed);
	return 1;
}

// ---- video frame ring ----
static inline unsigned vf_count(void) { return vf_write - vf_read; }
// reserve the next write slot if there's room; returns slot or -1
static int vf_reserve(void) {
	if (vf_count() >= FRAME_SLOTS) return -1;
	return (int)(vf_write % FRAME_SLOTS);
}
static void vf_commit(int index) {
	frame_idx[vf_write % FRAME_SLOTS] = index;
	vf_write++;
}

static void blit_to_target(const uint8_t* fr) {
	if (!target) return;
	int w, h, rb; uint8_t* data = NULL; uint8_t* mask = NULL;
	pd->graphics->getBitmapData(target, &w, &h, &rb, &mask, &data);
	if (!data) return;
	int srcrb = VW / 8;                 // 50
	int rows = h < VH ? h : VH;
	int copy = rb < srcrb ? rb : srcrb;
	for (int y = 0; y < rows; y++) memcpy(data + y * rb, fr + y * srcrb, copy);
}

static void reset_all(void) {
	atomic_store_explicit(&ring_write, 0, memory_order_relaxed);
	atomic_store_explicit(&ring_read, 0, memory_order_relaxed);
	atomic_store_explicit(&pcm_write, 0, memory_order_relaxed);
	atomic_store_explicit(&pcm_read, 0, memory_order_relaxed);
	atomic_store_explicit(&samples_played, 0, memory_order_relaxed);
	vf_write = vf_read = 0; vfill = 0;
	mp3dec_init(&mp3dec);
	rs_in_rate = 0; rs_pos_q16 = 0; rs_prev = 0;
	have_header = 0; started = 0; ready = 0; bytes_total = 0; last_shown = -1;
	net_done = 0; v_completed = 0;
	ps = PS_HEADER; hpos = 0; body_remaining = 0;
}

static void teardown(void) {
	if (vsource) { pd->sound->removeSource(vsource); vsource = NULL; }
	started = 0; ready = 0;
}

// ---- parser: returns bytes consumed from `in` (backpressure stops early) ----
static unsigned parse(const uint8_t* in, unsigned len) {
	unsigned i = 0;
	while (i < len) {
		if (ps == PS_HEADER) {
			while (hpos < 16 && i < len) hbuf[hpos++] = in[i++];
			if (hpos < 16) break;
			if (memcmp(hbuf, "RWLPVID1", 8) != 0) { ps = PS_PKTHEAD; hpos = 0; break; } // tolerate
			v_fps = hbuf[12] | (hbuf[13] << 8);
			if (v_fps < 1) v_fps = 6;
			have_header = 1; ps = PS_PKTHEAD; hpos = 0;
		} else if (ps == PS_PKTHEAD) {
			while (hpos < 5 && i < len) hbuf[hpos++] = in[i++];
			if (hpos < 5) break;
			uint8_t type = hbuf[0];
			body_remaining = (uint32_t)hbuf[1] | ((uint32_t)hbuf[2] << 8) |
			                 ((uint32_t)hbuf[3] << 16) | ((uint32_t)hbuf[4] << 24);
			hpos = 0;
			if (type == 2) ps = PS_AUDIO;
			else if (type == 1) {
				if (vf_reserve() < 0) { /* no slot: backpressure, rewind header */ hpos = 5; i -= 5; return i; }
				vfill = 0; ps = PS_VIDEO;
			} else { ps = PS_AUDIO; } // unknown -> skip as audio (drains)
		} else if (ps == PS_AUDIO) {
			unsigned avail = len - i, chunk = body_remaining < avail ? body_remaining : avail;
			unsigned pushed = ring_push(in + i, chunk);
			i += pushed; body_remaining -= pushed;
			if (pushed < chunk) return i;            // ring full: backpressure
			if (body_remaining == 0) ps = PS_PKTHEAD;
		} else { // PS_VIDEO
			int slot = (int)(vf_write % FRAME_SLOTS);
			unsigned avail = len - i;
			unsigned need = body_remaining < avail ? (unsigned)body_remaining : avail;
			unsigned space = (vfill < FRAME_BYTES) ? (FRAME_BYTES - vfill) : 0;
			unsigned take = need < space ? need : space;
			memcpy(frames[slot] + vfill, in + i, take);
			vfill += take; i += take; body_remaining -= take;
			if (body_remaining == 0) {
				// frame complete (index = how many frames committed so far)
				vf_commit((int)vf_write);
				ps = PS_PKTHEAD;
			}
		}
	}
	return i;
}

// ---- Lua surface ----
static int l_start(lua_State* L) { (void)L; teardown(); reset_all();
	pd->system->logToConsole("streamvideo: start"); pd->lua->pushBool(1); return 1; }

static int l_setTarget(lua_State* L) {
	target = pd->lua->getBitmap(1);
	pd->lua->pushBool(target != NULL); return 1;
}

static int l_feed(lua_State* L) {
	size_t len = 0; const char* in = pd->lua->getArgBytes(1, &len);
	if (!in || len == 0) { pd->lua->pushInt(0); return 1; }
	unsigned consumed = parse((const uint8_t*)in, (unsigned)len);
	bytes_total += consumed;
	pd->lua->pushInt((int)consumed); return 1;
}

static int l_tick(lua_State* L) {
	(void)L;
	decodeIntoPCM(8);
	// start audio once we have a little buffered
	if (!started && ring_used() >= PREBUFFER_BYTES) {
		vsource = pd->sound->addSource(audioCallback, NULL, 0);
		started = 1;
	}
	// target frame from the audio clock
	unsigned sp = atomic_load_explicit(&samples_played, memory_order_relaxed);
	int target_idx = started ? (int)(((int64_t)sp * v_fps) / OUTPUT_HZ) : 0;
	// advance through ready frames up to target, keep the last one
	int show_slot = -1, show_idx = -1;
	while (vf_count() > 0) {
		unsigned s = vf_read % FRAME_SLOTS;
		if (frame_idx[s] <= target_idx + 1) { show_slot = (int)s; show_idx = frame_idx[s]; vf_read++; }
		else break;
	}
	if (show_slot >= 0 && show_idx != last_shown) {
		blit_to_target(frames[show_slot]);
		last_shown = show_idx;
		ready = 1;
	} else if (!ready && vf_count() > 0) {
		// nothing started yet: show the first available frame so it isn't blank
		unsigned s = vf_read % FRAME_SLOTS;
		blit_to_target(frames[s]); ready = 1;
	}
	// End-of-film: Lua said the feed is done AND everything's drained (no encoded
	// bytes, no buffered frames, no PCM left to play). The film played all the way.
	if (net_done && started && !v_completed && ring_used() == 0 && vf_count() == 0) {
		unsigned pr = atomic_load_explicit(&pcm_read,  memory_order_acquire);
		unsigned pw = atomic_load_explicit(&pcm_write, memory_order_acquire);
		if (pr == pw) v_completed = 1;
	}
	pd->lua->pushBool(1); return 1;
}

// Flow control: true only if there's comfortable room for more (so Lua doesn't
// flood the engine when the network is faster than playback).
static int l_room(lua_State* L) { (void)L;
	int ok = (ring_free() > 16384) && (vf_count() < (unsigned)(FRAME_SLOTS - 2));
	pd->lua->pushBool(ok); return 1; }

static int l_stop(lua_State* L) { (void)L; teardown(); pd->lua->pushBool(1); return 1; }
static int l_finalize(lua_State* L) { (void)L; net_done = 1; pd->lua->pushBool(1); return 1; }
static int l_isFinished(lua_State* L) { (void)L; pd->lua->pushBool(v_completed); return 1; }
static int l_isPlaying(lua_State* L) { (void)L; pd->lua->pushBool(started); return 1; }
static int l_isReady(lua_State* L) { (void)L; pd->lua->pushBool(ready); return 1; }
static int l_posSeconds(lua_State* L) { (void)L;
	unsigned sp = atomic_load_explicit(&samples_played, memory_order_relaxed);
	pd->lua->pushInt((int)(sp / OUTPUT_HZ)); return 1; }
static int l_bytesText(lua_State* L) { (void)L;
	static char b[24];
	if (bytes_total < 1024*1024) snprintf(b, sizeof(b), "%u KB", bytes_total/1024);
	else snprintf(b, sizeof(b), "%.1f MB", (double)bytes_total/(1024.0*1024.0));
	pd->lua->pushString(b); return 1; }

// Called from streamaudio.c's eventHandler (the single SDK entry point).
int eventHandler_streamvideo(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
	(void)arg;
	if (event == kEventInitLua) {
		const char* err = NULL;
		#define REGV(fn, name) if (!pd->lua->addFunction(fn, name, &err)) \
			pd->system->logToConsole("streamvideo: addFunction %s failed: %s", name, err?err:"(null)");
		// pd is set by the audio engine's kEventInit; ensure we have it
		if (!pd) pd = playdate;
		REGV(l_start,      "streamvideo.startC");
		REGV(l_setTarget,  "streamvideo.setTargetC");
		REGV(l_feed,       "streamvideo.feedC");
		REGV(l_tick,       "streamvideo.tickC");
		REGV(l_room,       "streamvideo.roomC");
		REGV(l_stop,       "streamvideo.stopC");
		REGV(l_finalize,   "streamvideo.finalizeC");
		REGV(l_isFinished, "streamvideo.isFinishedC");
		REGV(l_isPlaying,  "streamvideo.isPlayingC");
		REGV(l_isReady,    "streamvideo.isReadyC");
		REGV(l_posSeconds, "streamvideo.posSecondsC");
		REGV(l_bytesText,  "streamvideo.bytesTextC");
		#undef REGV
	}
	return 0;
}

void streamvideo_setPD(PlaydateAPI* p) { pd = p; }
