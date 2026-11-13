// Tuner's TV engine: plays one channel with sound, synced to the audio clock; snow and the tuning picture while it
// connects, scanline/CRT overlays and a stall watchdog. The app draws menus and banners over it (themes.c).
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <sys/stat.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "channels.h"
#include "store.h"
#include "tt.h"
#include "tv.h"
#include "ui.h"

#define SCREEN_W 1024
#define SCREEN_H 768
#define AUDIO_RATE 48000
#define VQ 6        // decoded video frames buffered ahead
#define VPKTS 512   // video packets buffered between demuxer and decoder
#define AUDIO_AHEAD 1.0   // seconds of decoded audio the demuxer keeps queued
#define AUDIO_START 0.3   // seconds queued before sound (and the clock) starts
#define AUDIO_START_HEAVY 1.5   // ... for streams above 720p (big segments, Wi-Fi needs a head start)

typedef struct {
  // set by the decode thread, read by the main loop (under lock)
  SDL_mutex *lock;
  AVFrame *frames[VQ]; int head, count;   // ring of decoded, display-ready yuv420p frames
  int w, h; double sar;                   // video size and sample aspect ratio
  double audio_pts;                       // pts (s) of the end of the audio queued so far
  int connected, failed, stop;
  // video packets from the demuxer to the video decoder (so a full frame ring never blocks the audio)
  AVPacket *vpkts[VPKTS]; int vp_head, vp_count;
  SDL_cond *vp_cond;
  int has_audio, eof, audio_started;
  int heavy;   // > 720p: buffer more before starting
  Uint32 last_frame_at;   // SDL ticks of the last decoded video frame (stall watchdog)
  char error[128];
  const char *url;
  const char *user_agent;
  SDL_AudioDeviceID audio;
} Stream;

// Stats, logged every 2 s: frames decoded/shown/dropped, render loop rate, upload/present time, queue depths.
static int st_decoded, st_shown, st_dropped, st_loops, st_jumps;
static double st_upload_ms, st_present_ms;

static double frame_pts(AVFrame *f, AVStream *st) {
  int64_t p = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  return p == AV_NOPTS_VALUE ? 0 : p * av_q2d(st->time_base);
}

static int interrupt_cb(void *opaque) { return ((Stream *)opaque)->stop; }

typedef struct { Stream *s; AVCodecContext *vc; AVStream *vst; } VideoJob;

// Video decoder: packets from the queue -> frames in the ring (waits while the ring is full).
static int video_thread(void *arg) {
  VideoJob *j = arg; Stream *s = j->s;
  struct SwsContext *sws = NULL;
  AVFrame *fr = av_frame_alloc();
  while (!s->stop) {
    SDL_LockMutex(s->lock);
    while (!s->stop && !s->vp_count && !s->eof) SDL_CondWaitTimeout(s->vp_cond, s->lock, 50);
    if (s->stop || (!s->vp_count && s->eof)) { SDL_UnlockMutex(s->lock); break; }
    AVPacket *pkt = s->vpkts[s->vp_head]; s->vp_head = (s->vp_head + 1) % VPKTS; s->vp_count--;
    SDL_UnlockMutex(s->lock);
    if (avcodec_send_packet(j->vc, pkt) == 0) {
      while (avcodec_receive_frame(j->vc, fr) == 0) {
        AVFrame *out = av_frame_alloc();
        if (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_YUVJ420P) av_frame_ref(out, fr);
        else {  // e.g. 10-bit or 4:2:2: convert to plain yuv420p for the texture
          sws = sws_getCachedContext(sws, fr->width, fr->height, fr->format, fr->width, fr->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
          out->format = AV_PIX_FMT_YUV420P; out->width = fr->width; out->height = fr->height;
          av_frame_get_buffer(out, 32);
          sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, out->data, out->linesize);
        }
        out->pts = (int64_t)(frame_pts(fr, j->vst) * 1000000);  // microseconds
        // wait for room in the ring
        for (;;) {
          SDL_LockMutex(s->lock);
          if (s->stop || s->count < VQ) break;
          SDL_UnlockMutex(s->lock); SDL_Delay(3);
        }
        if (s->stop) { SDL_UnlockMutex(s->lock); av_frame_free(&out); break; }
        s->frames[(s->head + s->count) % VQ] = out; s->count++; st_decoded++;
        s->w = fr->width; s->h = fr->height;
        s->sar = fr->sample_aspect_ratio.num ? av_q2d(fr->sample_aspect_ratio) : 1.0;
        s->connected = 1; s->last_frame_at = SDL_GetTicks();
        SDL_UnlockMutex(s->lock);
      }
    }
    av_packet_free(&pkt);
  }
  av_frame_free(&fr);
  if (sws) sws_freeContext(sws);
  return 0;
}

// Demuxer: reads the stream, queues video packets for video_thread and decodes audio straight into SDL's queue
// (keeping about AUDIO_AHEAD seconds there).
static int decode_thread(void *arg) {
  Stream *s = arg;
  AVFormatContext *fmt = avformat_alloc_context();
  fmt->interrupt_callback.callback = interrupt_cb; fmt->interrupt_callback.opaque = s;
  AVDictionary *opt = NULL;
  av_dict_set(&opt, "rw_timeout", "10000000", 0);
  av_dict_set(&opt, "reconnect", "1", 0);
  if (s->user_agent && s->user_agent[0]) av_dict_set(&opt, "user_agent", s->user_agent, 0);
  if (avformat_open_input(&fmt, s->url, NULL, &opt) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
    snprintf(s->error, sizeof s->error, "no signal"); s->failed = 1; return 0;
  }
  // Video: the largest variant up to 720p (smallest one if all are bigger). Audio: the one ffmpeg likes best.
  int vi = -1, best = -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    AVCodecParameters *p = fmt->streams[i]->codecpar;
    if (p->codec_type != AVMEDIA_TYPE_VIDEO || !p->width) continue;
    int score = p->height <= 720 ? p->width * p->height : -(p->width * p->height);
    if (vi < 0 || score > best) { best = score; vi = i; }
  }
  int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
  if (vi < 0) { snprintf(s->error, sizeof s->error, "no video"); s->failed = 1; avformat_close_input(&fmt); return 0; }
  for (unsigned i = 0; i < fmt->nb_streams; i++) fmt->streams[i]->discard = ((int)i == vi || (int)i == ai) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

  AVStream *vst = fmt->streams[vi];
  const AVCodec *vdec = avcodec_find_decoder(vst->codecpar->codec_id);
  AVCodecContext *vc = avcodec_alloc_context3(vdec);
  avcodec_parameters_to_context(vc, vst->codecpar);
  vc->thread_count = 3; vc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  // Above 720p the Brick is near its limit: skip H.264/H.265 deblocking (hardly visible on a 3.2" screen).
  if (vst->codecpar->height > 720) { vc->skip_loop_filter = AVDISCARD_ALL; s->heavy = 1; }
  avcodec_open2(vc, vdec, NULL);

  AVStream *ast = ai >= 0 ? fmt->streams[ai] : NULL;
  AVCodecContext *ac = NULL; SwrContext *swr = NULL;
  if (ast) {
    const AVCodec *adec = avcodec_find_decoder(ast->codecpar->codec_id);
    ac = avcodec_alloc_context3(adec);
    avcodec_parameters_to_context(ac, ast->codecpar);
    if (!adec || avcodec_open2(ac, adec, NULL) < 0) { avcodec_free_context(&ac); ast = NULL; }
  }
  s->has_audio = ast != NULL;

  VideoJob job = { s, vc, vst };
  SDL_Thread *vt = SDL_CreateThread(video_thread, "video", &job);

  AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
  uint8_t *abuf = NULL; int abuf_size = 0;
  while (!s->stop) {
    // Enough read ahead? (audio well queued and video packets waiting, or the packet queue full)
    SDL_LockMutex(s->lock); int vp = s->vp_count; SDL_UnlockMutex(s->lock);
    double aq = (double)SDL_GetQueuedAudioSize(s->audio) / (AUDIO_RATE * 4);
    if (vp >= VPKTS - 1 || ((!ast || aq >= AUDIO_AHEAD) && vp >= 30)) { SDL_Delay(5); continue; }
    if (av_read_frame(fmt, pkt) < 0) { if (!s->connected) { snprintf(s->error, sizeof s->error, "no signal"); s->failed = 1; } break; }
    if (pkt->stream_index == vi) {
      SDL_LockMutex(s->lock);
      s->vpkts[(s->vp_head + s->vp_count) % VPKTS] = av_packet_clone(pkt); s->vp_count++;
      SDL_CondSignal(s->vp_cond);
      SDL_UnlockMutex(s->lock);
    } else if (ast && pkt->stream_index == ai && avcodec_send_packet(ac, pkt) == 0) {
      while (avcodec_receive_frame(ac, fr) == 0) {
        if (!swr) {
          AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
          swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE, &fr->ch_layout, fr->format, fr->sample_rate, 0, NULL);
          swr_init(swr);
        }
        int n = swr_get_out_samples(swr, fr->nb_samples);
        if (n * 4 > abuf_size) { abuf_size = n * 4; abuf = realloc(abuf, abuf_size); }
        int got = swr_convert(swr, &abuf, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
        if (got > 0) {
          if (!s->audio_started) { SDL_ClearQueuedAudio(s->audio); s->audio_started = 1; }  // drop the snow hiss
          SDL_QueueAudio(s->audio, abuf, got * 4);
          SDL_LockMutex(s->lock);
          s->audio_pts = frame_pts(fr, ast) + (double)fr->nb_samples / fr->sample_rate;
          SDL_UnlockMutex(s->lock);
        }
      }
    }
    av_packet_unref(pkt);
  }
  SDL_LockMutex(s->lock); s->eof = 1; SDL_CondSignal(s->vp_cond); SDL_UnlockMutex(s->lock);
  SDL_WaitThread(vt, NULL);
  SDL_LockMutex(s->lock);
  while (s->vp_count) { av_packet_free(&s->vpkts[s->vp_head]); s->vp_head = (s->vp_head + 1) % VPKTS; s->vp_count--; }
  SDL_UnlockMutex(s->lock);
  av_packet_free(&pkt); av_frame_free(&fr); free(abuf);
  if (swr) swr_free(&swr);
  avcodec_free_context(&vc); if (ac) avcodec_free_context(&ac);
  avformat_close_input(&fmt);
  return 0;
}


// --- picture overlays (L1/R1): scanlines and a CRT look, pre-drawn once as screen-size transparent layers --------
static const char *OVERLAY_NAMES[] = { "OFF", "SCANLINES LIGHT", "SCANLINES STRONG", "CRT" };
#define NOVERLAYS 4
static SDL_Texture *make_overlay(SDL_Renderer *r, int kind) {
  if (kind == 0) return NULL;
  SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, SCREEN_W, SCREEN_H);
  uint32_t *px = malloc(SCREEN_W * SCREEN_H * 4);
  for (int y = 0; y < SCREEN_H; y++) for (int x = 0; x < SCREEN_W; x++) {
    // scanlines: every other screen row darkened (384 lines, close to a TV's visible lines)
    int a = (y & 1) ? (kind == 1 ? 70 : kind == 2 ? 140 : 110) : 0;
    uint32_t rgb = 0;
    if (kind == 3) {
      // aperture grille: faint red/green/blue column tint
      static const uint32_t mask[3] = { 0x00300000, 0x00003000, 0x00000030 };
      rgb = mask[x % 3];
      a = a + 18;
      // vignette: darker towards the corners
      double dx = (x - SCREEN_W / 2.0) / (SCREEN_W / 2.0), dy = (y - SCREEN_H / 2.0) / (SCREEN_H / 2.0);
      double v = dx * dx * 0.55 + dy * dy * 0.55; v = v * v;
      a += (int)(v * 255 * 0.85);
      if (a > 255) a = 255;
    }
    px[y * SCREEN_W + x] = (uint32_t)a << 24 | rgb;
  }
  SDL_UpdateTexture(t, NULL, px, SCREEN_W * 4);
  free(px);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  return t;
}

// --- snow ---------------------------------------------------------------------------------------------------------
#define SNOW_W 256
#define SNOW_H 192
static void draw_snow(SDL_Renderer *r, SDL_Texture *t) {
  static uint32_t seed = 12345;
  void *pix; int pitch;
  SDL_LockTexture(t, NULL, &pix, &pitch);
  for (int y = 0; y < SNOW_H; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)pix + y * pitch);
    for (int x = 0; x < SNOW_W; x++) {
      seed = seed * 1664525u + 1013904223u;
      uint8_t v = seed >> 24;
      row[x] = 0xFF000000u | v << 16 | v << 8 | v;
    }
  }
  SDL_UnlockTexture(t);
  SDL_RenderCopy(r, t, NULL, NULL);
}
static void snow_audio(SDL_AudioDeviceID dev) {  // keep ~0.1 s of hiss queued
  static uint32_t seed = 777;
  if (SDL_GetQueuedAudioSize(dev) > AUDIO_RATE * 4 / 10) return;
  int16_t buf[2 * 1024];
  for (int i = 0; i < 2 * 1024; i += 2) { seed = seed * 1664525u + 1013904223u; buf[i] = buf[i + 1] = (int16_t)((int32_t)(seed >> 16) - 32768) / 10; }
  SDL_QueueAudio(dev, buf, sizeof buf);
}


// --- tuning images: the programme's image from the guide (or the channel logo), fetched in the background,
// cached on the card, shown under the snow while a channel connects --------------------------------------------
typedef struct { char url[256]; char ua[128]; SDL_Surface *surface; int done, cancelled; } ImageJob;

static unsigned long hash_str(const char *s) { unsigned long h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }

static int image_thread(void *arg) {
  ImageJob *j = arg;
  char path[700]; snprintf(path, sizeof path, "%s/cache/%lx.img", data_dir, hash_str(j->url));
  SDL_RWops *rw = SDL_RWFromFile(path, "rb");
  if (!rw) {   // not cached: download it (ffmpeg's https), then cache
    AVIOContext *io = NULL; AVDictionary *opt = NULL;
    av_dict_set(&opt, "rw_timeout", "6000000", 0);
    if (j->ua[0]) av_dict_set(&opt, "user_agent", j->ua, 0);
    if (avio_open2(&io, j->url, AVIO_FLAG_READ, NULL, &opt) >= 0) {
      uint8_t *buf = NULL; int size = 0, cap = 0, n;
      uint8_t chunk[16384];
      while (!j->cancelled && (n = avio_read(io, chunk, sizeof chunk)) > 0 && size < 4 << 20) {
        if (size + n > cap) { cap = (size + n) * 2; buf = realloc(buf, cap); }
        memcpy(buf + size, chunk, n); size += n;
      }
      avio_closep(&io);
      if (size > 0 && !j->cancelled) {
        FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, size, f); fclose(f); }
        rw = SDL_RWFromFile(path, "rb");
      }
      free(buf);
    }
    av_dict_free(&opt);
  }
  if (rw && !j->cancelled) j->surface = IMG_Load_RW(rw, 1); else if (rw) SDL_RWclose(rw);
  j->done = 1;
  return 0;
}

static ImageJob *fetch_image(const Channel *c) {
  const Programme *cur, *next;
  now_next(c, time(NULL), &cur, &next);
  const char *url = cur && cur->icon[0] ? cur->icon : c->art[0] ? c->art : c->logo;
  if (!url[0]) return NULL;
  ImageJob *j = calloc(1, sizeof *j);
  snprintf(j->url, sizeof j->url, "%s", url); snprintf(j->ua, sizeof j->ua, "%s", c->user_agent);
  SDL_DetachThread(SDL_CreateThread(image_thread, "image", j));
  return j;   // leaked when cancelled (tiny), like Stream, so the thread never touches freed memory
}

// The image, jittering sideways now and then, with snow over it that thins out as the seconds pass.
static void draw_tuning(SDL_Renderer *r, SDL_Texture *img, int iw, int ih, SDL_Texture *snow, Uint32 since_ms) {
  static uint32_t seed = 99;
  if (since_ms > 6000) {   // a long wait: the test card, as stations showed between programmes
    draw_testcard();
    if ((since_ms / 600) % 2) tt_draw((SCREEN_W - tt_text_width(30, "SEARCHING FOR SIGNAL")) / 2, SCREEN_H - 90, 30, "SEARCHING FOR SIGNAL", TT_PALETTE[TT_WHITE]);
    return;
  }
  if (img) {
    double aspect = (double)iw / ih, sw = SCREEN_W, sh = SCREEN_W / aspect;
    if (sh > SCREEN_H) { sh = SCREEN_H; sw = SCREEN_H * aspect; }
    seed = seed * 1664525u + 1013904223u;
    int jitter = (seed >> 28) < 3 ? (int)((seed >> 20) % 24) - 12 : 0;   // occasional horizontal slip
    SDL_Rect dst = { (int)((SCREEN_W - sw) / 2) + jitter, (int)((SCREEN_H - sh) / 2), (int)sw, (int)sh };
    SDL_RenderCopy(r, img, NULL, &dst);
  }
  // snow: thick at first, thinning towards a steady haze as if the tuner is locking on
  double t = since_ms / 1000.0;
  int alpha = img ? (int)(235 - (t > 2.5 ? 2.5 : t) * 50) : 255;
  SDL_SetTextureBlendMode(snow, SDL_BLENDMODE_BLEND);
  SDL_SetTextureAlphaMod(snow, alpha);
  draw_snow(r, snow);
  SDL_SetTextureAlphaMod(snow, 255);
}

static Stream *start_stream(const Channel *c, SDL_AudioDeviceID audio) {
  Stream *s = calloc(1, sizeof *s);
  s->lock = SDL_CreateMutex(); s->vp_cond = SDL_CreateCond(); s->url = c->url; s->user_agent = c->user_agent; s->audio = audio; s->sar = 1;
  SDL_ClearQueuedAudio(audio);
  SDL_DetachThread(SDL_CreateThread(decode_thread, "decode", s));
  return s;
}
// Stopping a stream: the thread notices s->stop (also via ffmpeg's interrupt callback) and exits on its own.
// The Stream is leaked on purpose (a few KB per zap) so the thread never touches freed memory.
static void stop_stream(Stream *s) {
  s->stop = 1;
  SDL_LockMutex(s->lock);
  while (s->count) { av_frame_free(&s->frames[s->head]); s->head = (s->head + 1) % VQ; s->count--; }
  SDL_UnlockMutex(s->lock);
}


// --- the engine: one channel at a time, drawn by tv_render() each frame -----------------------------------------
static SDL_Renderer *ren;
static SDL_AudioDeviceID audio;
static Settings *set;
static SDL_Texture *snow, *overlays[NOVERLAYS], *video;
static int tex_w, tex_h;
static Channel *cur_ch;
static Stream *s;
static ImageJob *ijob;
static SDL_Texture *itex; static int iw, ih;
static int retries, no_signal, playing;
static const char *fail_reason = "";   // why the channel gave up, in plain words
static Uint32 started_at, changed_at;
static double wall_clock, video_offset; static Uint64 wall_at;
static int osd_frames, osd_drops; static Uint32 osd_at; static double osd_fps;

void tv_setup(SDL_Renderer *r, SDL_AudioDeviceID a, Settings *st) {
  ren = r; audio = a; set = st;
  snow = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SNOW_W, SNOW_H);
  for (int i = 0; i < NOVERLAYS; i++) overlays[i] = make_overlay(ren, i);
  IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
}

void tv_tune(Channel *c) {
  if (s) { stop_stream(s); s = NULL; }
  if (ijob) { ijob->cancelled = 1; ijob = NULL; }
  if (itex) { SDL_DestroyTexture(itex); itex = NULL; }
  SDL_PauseAudioDevice(audio, 1); SDL_ClearQueuedAudio(audio);
  cur_ch = c;
  retries = no_signal = playing = 0; wall_at = 0; video_offset = 0;
  started_at = changed_at = SDL_GetTicks();
  if (!c) { if (video) { SDL_DestroyTexture(video); video = NULL; } return; }
  s = start_stream(c, audio);
  ijob = fetch_image(c);
  fprintf(stderr, "tune: %s (%s)\n", c->name, c->url);
}

Channel *tv_channel(void) { return cur_ch; }
int tv_state(void) { return !cur_ch ? TV_OFF : no_signal ? TV_NO_SIGNAL : playing ? TV_PLAYING : TV_TUNING; }
Uint32 tv_tuned_ms(void) { return SDL_GetTicks() - changed_at; }
void tv_overlay_step(int dir) { set->overlay = (set->overlay + dir + NOVERLAYS) % NOVERLAYS; settings_save(set); }
const char *tv_overlay_name(void) { return OVERLAY_NAMES[set->overlay % NOVERLAYS]; }
void tv_osd_line(char *buf, int n) {
  int w = 0, h = 0;
  if (s) { SDL_LockMutex(s->lock); w = s->w; h = s->h; SDL_UnlockMutex(s->lock); }
  snprintf(buf, n, "%.0f FPS  %dX%d  DROP %d  RETRY %d", osd_fps, w, h, osd_drops, retries);
}

// Watchdog, start buffer and A/V sync, then the picture (or the tuning screen, or NO SIGNAL) and the overlay.
void tv_render(void) {
  if (!cur_ch) return;
  // Stall watchdog: reconnect when a stream fails to open, stops delivering frames for 5 s, or hasn't shown a
  // picture 15 s after starting. After 3 tries in a row: NO SIGNAL (and the channel is marked dead for a week).
  if (!no_signal) {
    Uint32 now_t = SDL_GetTicks();
    SDL_LockMutex(s->lock); int conn = s->connected; Uint32 last = s->last_frame_at; SDL_UnlockMutex(s->lock);
    const char *why = s->failed ? s->error : (conn && now_t - last > 5000) ? "stalled" : (!conn && now_t - started_at > 15000) ? "timeout" : NULL;
    if (why) {
      if (retries >= 3) {
        no_signal = 1; mark_dead(cur_ch->key);
        fail_reason = !strcmp(why, "stalled") ? "The stream stopped." : !strcmp(why, "timeout") ? "No picture after 15 seconds." :
                      !strcmp(why, "no video") ? "There is no video in this stream." : "The channel could not be opened."; fprintf(stderr, "%s: giving up (%s)\n", cur_ch->name, why); SDL_PauseAudioDevice(audio, 1); SDL_ClearQueuedAudio(audio); }
      else {
        retries++;
        fprintf(stderr, "%s: %s, reconnecting (try %d)\n", cur_ch->name, why, retries);
        stop_stream(s); s = start_stream(cur_ch, audio); started_at = now_t; playing = 0; wall_at = 0;
      }
    } else if (conn && now_t - last < 1000 && now_t - started_at > 30000) retries = 0;  // healthy again
  }
  if (ijob && ijob->done) {
    if (ijob->surface) { itex = SDL_CreateTextureFromSurface(ren, ijob->surface); iw = ijob->surface->w; ih = ijob->surface->h; SDL_FreeSurface(ijob->surface); }
    free(ijob); ijob = NULL;
  }
  // Start buffer: playback (sound and the clock) starts once enough is queued; until then the tuning picture
  // stays up. Heavy streams (> 720p) get a longer head start.
  double queued = (double)SDL_GetQueuedAudioSize(audio) / (AUDIO_RATE * 4);
  if (!playing && !no_signal && s->connected) {
    SDL_LockMutex(s->lock); int frames = s->count; SDL_UnlockMutex(s->lock);
    double need = s->heavy ? AUDIO_START_HEAVY : AUDIO_START;
    if ((s->has_audio && queued >= need) || (!s->has_audio && frames >= (s->heavy ? VQ : 1))) { playing = 1; SDL_PauseAudioDevice(audio, 0); }
    else if (s->audio_started) SDL_PauseAudioDevice(audio, 1);
  }
  if (no_signal) {
    SDL_SetRenderDrawColor(ren, 0, 0, 170, 255); SDL_RenderClear(ren);   // VCR blue
    tt_draw((SCREEN_W - tt_text_width(64, "NO SIGNAL")) / 2, SCREEN_H / 2 - 110, 64, "NO SIGNAL", TT_PALETTE[TT_WHITE]);
    char nm[64]; snprintf(nm, sizeof nm, "%s", cur_ch->name);
    tt_draw((SCREEN_W - tt_text_width(32, nm)) / 2, SCREEN_H / 2 - 20, 32, nm, TT_PALETTE[TT_YELLOW]);
    tt_draw((SCREEN_W - tt_text_width(26, fail_reason)) / 2, SCREEN_H / 2 + 30, 26, fail_reason, TT_PALETTE[TT_WHITE]);
    const char *hint = "Up/down: another channel";
    tt_draw((SCREEN_W - tt_text_width(22, hint)) / 2, SCREEN_H / 2 + 90, 22, hint, TT_PALETTE[TT_CYAN]);
  } else if (!s->connected || !playing) {
    if (!set->plain_tuning) {   // plain: a black screen; the app draws the channel info and LOADING over it
      draw_tuning(ren, itex, iw, ih, snow, SDL_GetTicks() - changed_at);
      if (!s->audio_started) { SDL_PauseAudioDevice(audio, 0); snow_audio(audio); }
    }
    if (retries) {   // reconnecting: say so over the snow
      char m[48]; snprintf(m, sizeof m, "SIGNAL LOST - RETRYING (%d/3)", retries);
      tt_draw((SCREEN_W - tt_text_width(28, m)) / 2, 40, 28, m, TT_PALETTE[TT_YELLOW]);
    }
  } else {
    // Clock: the audio being heard (end of queued audio minus what's still queued). If the audio runs dry (or there
    // is none), the clock runs on the wall clock so the picture never freezes waiting for sound.
    SDL_LockMutex(s->lock);
    double clock;
    Uint64 nowc = SDL_GetPerformanceCounter();
    if (s->has_audio && queued > 0.02) { clock = s->audio_pts - queued; wall_clock = clock; wall_at = nowc; }
    else {
      if (!wall_at) { wall_clock = s->count ? s->frames[s->head]->pts / 1e6 : 0; wall_at = nowc; }
      clock = wall_clock + (double)(nowc - wall_at) / SDL_GetPerformanceFrequency();
    }
    AVFrame *show = NULL;
    // A frame more than 1.5 s from the clock means the timeline jumped (FAST channels splice in ads with
    // #EXT-X-DISCONTINUITY and their timestamps restart): show it now and re-anchor (video_offset).
    while (s->count) {
      double d = s->frames[s->head]->pts / 1e6 + video_offset - clock;
      if (d > 1.5 || d < -1.5) {
        st_jumps++; video_offset = clock - s->frames[s->head]->pts / 1e6;
        if (show) { av_frame_free(&show); st_dropped++; }
        show = s->frames[s->head]; s->head = (s->head + 1) % VQ; s->count--;
        break;
      }
      if (d > 0.01) break;
      if (show) { av_frame_free(&show); st_dropped++; osd_drops++; }
      show = s->frames[s->head]; s->head = (s->head + 1) % VQ; s->count--;
    }
    int w = s->w, h = s->h; double sar = s->sar;
    SDL_UnlockMutex(s->lock);
    if (show) {
      if (!video || tex_w != show->width || tex_h != show->height) {
        if (video) SDL_DestroyTexture(video);
        video = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, show->width, show->height);
        tex_w = show->width; tex_h = show->height;
      }
      Uint64 t = SDL_GetPerformanceCounter();
      SDL_UpdateYUVTexture(video, NULL, show->data[0], show->linesize[0], show->data[1], show->linesize[1], show->data[2], show->linesize[2]);
      st_upload_ms += (SDL_GetPerformanceCounter() - t) * 1000.0 / SDL_GetPerformanceFrequency();
      av_frame_free(&show); st_shown++; osd_frames++;
    }
    if (video && h) {  // fit to the 4:3 screen, letterboxing 16:9
      double aspect = (double)w * sar / h, sw = SCREEN_W, sh = SCREEN_W / aspect;
      if (sh > SCREEN_H) { sh = SCREEN_H; sw = SCREEN_H * aspect; }
      SDL_Rect dst = { (int)((SCREEN_W - sw) / 2), (int)((SCREEN_H - sh) / 2), (int)sw, (int)sh };
      SDL_RenderCopy(ren, video, NULL, &dst);
    }
  }
  if (SDL_GetTicks() - osd_at >= 1000) { osd_fps = osd_frames * 1000.0 / (SDL_GetTicks() - osd_at + 1); osd_frames = 0; osd_at = SDL_GetTicks(); }
  if (overlays[set->overlay % NOVERLAYS]) SDL_RenderCopy(ren, overlays[set->overlay % NOVERLAYS], NULL, NULL);

  static Uint32 st_last = 0;
  Uint32 now = SDL_GetTicks();
  if (now - st_last >= 5000 && playing) {
    double sec = (now - st_last) / 1000.0;
    SDL_LockMutex(s->lock); int q = s->count, w = s->w, h = s->h; SDL_UnlockMutex(s->lock);
    fprintf(stderr, "stats %dx%d: decoded %.1f fps, shown %.1f fps, dropped %d, jumps %d, upload %.1f ms, vqueue %d, audio %.0f ms\n",
            w, h, st_decoded / sec, st_shown / sec, st_dropped, st_jumps, st_shown ? st_upload_ms / st_shown : 0, q,
            SDL_GetQueuedAudioSize(audio) * 1000.0 / (AUDIO_RATE * 4));
    st_decoded = st_shown = st_dropped = st_loops = st_jumps = 0; st_upload_ms = st_present_ms = 0; st_last = now;
  }
}
