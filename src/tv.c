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
#include "hwdec.h"

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
  // stats for nerds (set once by the decode thread, or counted)
  char vcodec[24], acodec[24]; double vfps; int arate, ach, variants, variant; long vbitrate; volatile int hw;
  // audio tracks: those that go with the chosen video; the one playing; a switch asked for (LEFT/RIGHT), or -1
  struct { int idx, ch; char lang[8], codec[16]; } track[8]; int ntracks, atrack; volatile int want_track;
  // copied from the settings and the channel when the stream starts
  int max_h; char langs[256], pref_lang[8]; int pref_pos;
  volatile long long bytes; volatile int damaged, decoded;
} Stream;

// Stats, logged every 2 s: frames decoded/shown/dropped, render loop rate, upload/present time, queue depths.
static int st_decoded, st_shown, st_dropped, st_loops, st_jumps, st_jumps_total;
static double st_upload_ms, st_present_ms;

static double frame_pts(AVFrame *f, AVStream *st) {
  int64_t p = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  return p == AV_NOPTS_VALUE ? 0 : p * av_q2d(st->time_base);
}

static int interrupt_cb(void *opaque) { return ((Stream *)opaque)->stop; }

typedef struct { Stream *s; AVCodecContext *vc; AVStream *vst; HwDec *hw; } VideoJob;
static Settings *set;   // the app's settings (tv_setup)
static int use_hw = 1;   // Settings > Hardware decoding
void tv_hwdec(int on) { use_hw = on; }

// Puts a decoded yuv420p frame (pts in microseconds) into the ring, waiting for room. Returns -1 when stopping.
static int ring_put(Stream *s, AVFrame *out, double sar, int damaged) {
  for (;;) {
    SDL_LockMutex(s->lock);
    if (s->stop || s->count < VQ) break;
    SDL_UnlockMutex(s->lock); SDL_Delay(3);
  }
  if (s->stop) { SDL_UnlockMutex(s->lock); av_frame_free(&out); return -1; }
  s->frames[(s->head + s->count) % VQ] = out; s->count++; st_decoded++; s->decoded++;
  if (damaged) s->damaged++;
  s->w = out->width; s->h = out->height; s->sar = sar;
  s->connected = 1; s->last_frame_at = SDL_GetTicks();
  SDL_UnlockMutex(s->lock);
  return 0;
}

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
    if (j->hw) {   // the video engine: hand it the packet, take what pictures are ready
      int r, err = 0, stopped = 0;
      for (int spins = 0; (r = hwdec_send(j->hw, pkt)) == AVERROR(EAGAIN) && !s->stop && spins < 500; spins++) {
        AVFrame *f; int n = 0;
        while (!stopped && (f = hwdec_receive(j->hw, &err))) { n++; if (ring_put(s, f, 1.0, 0) < 0) stopped = 1; }
        if (!n) SDL_Delay(2);
      }
      AVFrame *f;
      while (!stopped && r >= 0 && (f = hwdec_receive(j->hw, &err))) if (ring_put(s, f, 1.0, 0) < 0) stopped = 1;
      if (r < 0 && r != AVERROR(EAGAIN)) err = 1;
      if (err) {   // the engine gave up on this stream: on in software from the next key frame
        fprintf(stderr, "hwdec: failed, decoding in software\n");
        hwdec_close(j->hw); j->hw = NULL; s->hw = 0;
      }
      av_packet_free(&pkt);
      continue;
    }
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
        if (ring_put(s, out, fr->sample_aspect_ratio.num ? av_q2d(fr->sample_aspect_ratio) : 1.0,
                     fr->decode_error_flags || (fr->flags & AV_FRAME_FLAG_CORRUPT)) < 0) break;
      }
    }
    av_packet_free(&pkt);
  }
  av_frame_free(&fr);
  if (sws) sws_freeContext(sws);
  if (j->hw) { hwdec_close(j->hw); j->hw = NULL; }
  return 0;
}

// Demuxer: reads the stream, queues video packets for video_thread and decodes audio straight into SDL's queue
// (keeping about AUDIO_AHEAD seconds there).
// Languages: tags in streams are 2- or 3-letter (and Norwegian alone has five); the settings use 3-letter codes.
static const char *LANG_ALIASES[] = { "no nor nob nno nb nn", "sv swe", "da dan", "fi fin", "en eng", "de deu ger", "fr fra fre",
  "es spa", "it ita", "nl nld dut", "pt por", "pl pol", "is isl ice", "et est", "ar ara", "tr tur", "ru rus", NULL };
static const char *lang_group(const char *code, char *one, size_t n) {
  for (int i = 0; LANG_ALIASES[i]; i++) {
    const char *w = LANG_ALIASES[i]; size_t cl = strlen(code);
    while (*w) {
      size_t wl = strcspn(w, " ");
      if (wl == cl && !strncasecmp(w, code, cl)) return LANG_ALIASES[i];
      w += wl; while (*w == ' ') w++;
    }
  }
  snprintf(one, n, "%s", code); return one;
}
static int lang_match(const char *a, const char *b) {
  char x[8], y[8];
  return a[0] && b[0] && !strcmp(lang_group(a, x, sizeof x), lang_group(b, y, sizeof y));
}
static AVCodecContext *open_audio(AVStream *ast) {
  const AVCodec *adec = avcodec_find_decoder(ast->codecpar->codec_id);
  if (!adec) return NULL;
  AVCodecContext *ac = avcodec_alloc_context3(adec);
  avcodec_parameters_to_context(ac, ast->codecpar);
  if (avcodec_open2(ac, adec, NULL) < 0) avcodec_free_context(&ac);
  return ac;
}

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
  // Video: the largest variant up to Max quality (the smallest one if all are bigger).
  int vi = -1, best = -1, max_h = s->max_h > 0 ? s->max_h : 720;
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    AVCodecParameters *p = fmt->streams[i]->codecpar;
    if (p->codec_type != AVMEDIA_TYPE_VIDEO || !p->width) continue;
    int score = p->height <= max_h ? p->width * p->height : -(p->width * p->height);
    if (vi < 0 || score > best) { best = score; vi = i; }
  }
  if (vi < 0) { snprintf(s->error, sizeof s->error, "no video"); s->failed = 1; avformat_close_input(&fmt); return 0; }
  // Audio: the tracks that go with that video (in HLS, its variant's), then the one chosen here before on this
  // channel, else the first in your languages' order, else the one ffmpeg likes best.
  int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
  {
    AVProgram *prog = av_find_program_from_stream(fmt, NULL, vi);
    unsigned count = prog ? prog->nb_stream_indexes : fmt->nb_streams;
    for (unsigned k = 0; k < count && s->ntracks < 8; k++) {
      int i = prog ? (int)prog->stream_index[k] : (int)k;
      AVCodecParameters *p = fmt->streams[i]->codecpar;
      if (p->codec_type != AVMEDIA_TYPE_AUDIO || !avcodec_find_decoder(p->codec_id)) continue;
      int t = s->ntracks++;
      s->track[t].idx = i; s->track[t].ch = p->ch_layout.nb_channels;
      AVDictionaryEntry *e = av_dict_get(fmt->streams[i]->metadata, "language", NULL, 0);
      snprintf(s->track[t].lang, sizeof s->track[t].lang, "%s", e && strcmp(e->value, "und") ? e->value : "");
      snprintf(s->track[t].codec, sizeof s->track[t].codec, "%s", avcodec_get_name(p->codec_id));
    }
    int pick = -1;
    if (s->pref_lang[0]) for (int t = 0; t < s->ntracks && pick < 0; t++) if (lang_match(s->track[t].lang, s->pref_lang)) pick = t;
    if (pick < 0 && !s->pref_lang[0] && s->pref_pos >= 0 && s->pref_pos < s->ntracks) pick = s->pref_pos;
    if (pick < 0 && s->ntracks > 1) {
      char langs[256]; snprintf(langs, sizeof langs, "%s", s->langs);
      char *save = NULL;
      for (char *l = strtok_r(langs, ",", &save); l && pick < 0; l = strtok_r(NULL, ",", &save))
        for (int t = 0; t < s->ntracks && pick < 0; t++) if (lang_match(s->track[t].lang, l)) pick = t;
    }
    if (pick < 0) for (int t = 0; t < s->ntracks; t++) if (s->track[t].idx == ai) pick = t;
    if (pick < 0 && s->ntracks) pick = 0;
    if (pick >= 0) { ai = s->track[pick].idx; s->atrack = pick; } else ai = -1;
  }
  for (unsigned i = 0; i < fmt->nb_streams; i++) fmt->streams[i]->discard = ((int)i == vi || (int)i == ai) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

  AVStream *vst = fmt->streams[vi];
  const AVCodec *vdec = avcodec_find_decoder(vst->codecpar->codec_id);
  {   // what the stats-for-nerds panel shows about the stream (no addresses)
    int k = 0;
    for (unsigned i = 0; i < fmt->nb_streams; i++) if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && fmt->streams[i]->codecpar->width) { k++; if ((int)i == vi) s->variant = k; }
    s->variants = k;
    AVDictionaryEntry *e = av_dict_get(vst->metadata, "variant_bitrate", NULL, 0);
    s->vbitrate = e ? atol(e->value) : (long)vst->codecpar->bit_rate;
    s->vfps = vst->avg_frame_rate.num ? av_q2d(vst->avg_frame_rate) : vst->r_frame_rate.num ? av_q2d(vst->r_frame_rate) : 0;
    snprintf(s->vcodec, sizeof s->vcodec, "%s", avcodec_get_name(vst->codecpar->codec_id));
  }
  AVCodecContext *vc = avcodec_alloc_context3(vdec);
  avcodec_parameters_to_context(vc, vst->codecpar);
  vc->thread_count = 3; vc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  // Above 720p the Brick is near its limit: skip H.264/H.265 deblocking (hardly visible on a 3.2" screen).
  if (vst->codecpar->height > 720) { vc->skip_loop_filter = AVDISCARD_ALL; s->heavy = 1; }
  avcodec_open2(vc, vdec, NULL);

  AVStream *ast = ai >= 0 ? fmt->streams[ai] : NULL;
  AVCodecContext *ac = NULL; SwrContext *swr = NULL;
  if (ast && !(ac = open_audio(ast))) ast = NULL;
  s->has_audio = ast != NULL;
  if (ast) { snprintf(s->acodec, sizeof s->acodec, "%s", avcodec_get_name(ast->codecpar->codec_id)); s->arate = ast->codecpar->sample_rate; s->ach = ast->codecpar->ch_layout.nb_channels; }

  VideoJob job = { s, vc, vst, NULL };
  if (use_hw && (job.hw = hwdec_open(vst->codecpar, vst->time_base, s->vfps))) s->hw = 1;
  fprintf(stderr, "video: %s %dx%d, %s decoding\n", s->vcodec, vst->codecpar->width, vst->codecpar->height, s->hw ? "hardware" : "software");
  SDL_Thread *vt = SDL_CreateThread(video_thread, "video", &job);

  AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
  uint8_t *abuf = NULL; int abuf_size = 0;
  while (!s->stop) {
    int want = s->want_track;
    if (want >= 0 && want < s->ntracks && want != s->atrack) {   // LEFT/RIGHT: another audio track, the picture goes on
      s->want_track = -1;
      if (ai >= 0) fmt->streams[ai]->discard = AVDISCARD_ALL;
      if (ac) avcodec_free_context(&ac);
      if (swr) swr_free(&swr);
      ai = s->track[want].idx; s->atrack = want;
      fmt->streams[ai]->discard = AVDISCARD_DEFAULT;
      ast = fmt->streams[ai];
      if (!(ac = open_audio(ast))) ast = NULL;
      snprintf(s->acodec, sizeof s->acodec, "%s", s->track[want].codec);
      s->arate = ast ? ast->codecpar->sample_rate : 0; s->ach = s->track[want].ch;
      SDL_ClearQueuedAudio(s->audio);   // the old language's sound goes; the clock runs on the wall until the new one comes
      s->has_audio = ast != NULL;
    } else if (want >= 0) s->want_track = -1;
    // Enough read ahead? (audio well queued and video packets waiting, or the packet queue full)
    SDL_LockMutex(s->lock); int vp = s->vp_count; SDL_UnlockMutex(s->lock);
    double aq = (double)SDL_GetQueuedAudioSize(s->audio) / (AUDIO_RATE * 4);
    if (vp >= VPKTS - 1 || ((!ast || aq >= AUDIO_AHEAD) && vp >= 30)) { SDL_Delay(5); continue; }
    if (av_read_frame(fmt, pkt) < 0) { if (!s->connected) { snprintf(s->error, sizeof s->error, "no signal"); s->failed = 1; } break; }
    if (pkt->stream_index == vi || pkt->stream_index == ai) s->bytes += pkt->size;
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
typedef struct { char url[256]; char ua[128]; SDL_Surface *surface; int done, cancelled, is_logo; } ImageJob;

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
  j->is_logo = url == c->logo;
  SDL_DetachThread(SDL_CreateThread(image_thread, "image", j));
  return j;   // leaked when cancelled (tiny), like Stream, so the thread never touches freed memory
}

// The image, jittering sideways now and then, with snow over it that thins out as the seconds pass. Pictures fill
// the screen; a logo (or a small image, which usually is one) sits in the middle with room around it.
static void draw_tuning(SDL_Renderer *r, SDL_Texture *img, int iw, int ih, int logo, SDL_Texture *snow, Uint32 since_ms) {
  static uint32_t seed = 99;
  if (since_ms > 6000) {   // a long wait: the test card, as stations showed between programmes
    draw_testcard();
    if ((since_ms / 600) % 2) tt_draw((SCREEN_W - tt_text_width(30, "SEARCHING FOR SIGNAL")) / 2, SCREEN_H - 90, 30, "SEARCHING FOR SIGNAL", TT_PALETTE[TT_WHITE]);
    return;
  }
  if (img) {
    if (iw <= 400 && ih <= 400) logo = 1;
    double bw = logo ? SCREEN_W * 0.5 : SCREEN_W, bh = logo ? SCREEN_H * 0.45 : SCREEN_H;
    double aspect = (double)iw / ih, sw = bw, sh = bw / aspect;
    if (sh > bh) { sh = bh; sw = bh * aspect; }
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
  s->want_track = -1; s->max_h = set ? set->max_height : 720;
  if (set) snprintf(s->langs, sizeof s->langs, "%s", set->languages);
  s->pref_pos = audio_pref_get(c->key, s->pref_lang, sizeof s->pref_lang);
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
static SDL_Texture *itex; static int iw, ih, ilogo;
static int retries, no_signal, playing;
static const char *fail_reason = "";   // why the channel gave up, in plain words
static Uint32 started_at, changed_at;
static double wall_clock, video_offset; static Uint64 wall_at;
static int osd_frames, osd_drops; static Uint32 osd_at; static double osd_fps;
static double nerd_av, nerd_dec_fps, nerd_kbps, nerd_loop_fps; static int nerd_loops, nerd_dec_last; static long long nerd_bytes_last;

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
  fprintf(stderr, "tune: %s (%s)\n", c->name, c->source);   // not the URL: yours can hold a login
}

Channel *tv_channel(void) { return cur_ch; }
int tv_state(void) { return !cur_ch ? TV_OFF : no_signal ? TV_NO_SIGNAL : playing ? TV_PLAYING : TV_TUNING; }
Uint32 tv_tuned_ms(void) { return SDL_GetTicks() - changed_at; }
void tv_overlay_step(int dir) { set->overlay = (set->overlay + dir + NOVERLAYS) % NOVERLAYS; settings_save(set); }
const char *tv_overlay_name(void) { return OVERLAY_NAMES[set->overlay % NOVERLAYS]; }
// The device: CPU load (from /proc/stat) and temperature, read once a second while the panel is up; the Wi-Fi
// signal and speed come from `iw` in a thread of their own (running a program from the render loop could stutter).
static int sys_cpu = -1, sys_temp = -1;
static volatile int sys_wifi, sys_wifi_mbit; static volatile Uint32 nerd_seen;
static int wifi_thread(void *arg) {
  (void)arg;
  while (SDL_GetTicks() - nerd_seen < 5000) {
    FILE *f = popen("iw dev wlan0 link 2>/dev/null", "r");
    int sig = 0; double rate = 0;
    if (f) {
      char line[160];
      while (fgets(line, sizeof line, f)) { sscanf(line, " signal: %d", &sig); sscanf(line, " tx bitrate: %lf", &rate); }
      pclose(f);
    }
    sys_wifi = sig; sys_wifi_mbit = (int)rate;
    SDL_Delay(2000);
  }
  nerd_seen = 0;
  return 0;
}
static void sys_read(void) {
  static unsigned long long pbusy, ptotal;
  FILE *f = fopen("/proc/stat", "r");
  if (f) {
    unsigned long long v[8] = {0};
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
      unsigned long long total = 0; for (int i = 0; i < 8; i++) total += v[i];
      unsigned long long busy = total - v[3] - v[4];
      if (ptotal && total > ptotal) sys_cpu = (int)((busy - pbusy) * 100 / (total - ptotal));
      pbusy = busy; ptotal = total;
    }
    fclose(f);
  }
  if ((f = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))) { int t; if (fscanf(f, "%d", &t) == 1) sys_temp = t > 1000 ? t / 1000 : t; fclose(f); }
}
// Counted every frame: decoded frames, bytes and render loops per second.
static void nerd_tick(void) {
  static Uint32 at;
  nerd_loops++;
  Uint32 now = SDL_GetTicks();
  if (now - at < 1000) return;
  double sec = (now - at) / 1000.0; at = now;
  if (s) {
    nerd_dec_fps = (s->decoded - nerd_dec_last) / sec; nerd_dec_last = s->decoded;
    nerd_kbps = (s->bytes - nerd_bytes_last) * 8 / 1000.0 / sec; nerd_bytes_last = s->bytes;
  }
  nerd_loop_fps = nerd_loops / sec; nerd_loops = 0;
  if (nerd_seen) sys_read();
}
// "ENG AC-3 5.1" for a track
static void track_label(const Stream *st, int t, char *b, size_t n) {
  const char *c = st->track[t].codec;
  const char *cn = !strcmp(c, "ac3") ? "AC-3" : !strcmp(c, "eac3") ? "E-AC-3" : !strcmp(c, "aac") || !strcmp(c, "aac_latm") ? "AAC" : !strcmp(c, "mp2") ? "MP2" : !strcmp(c, "mp3") ? "MP3" : c;
  int ch = st->track[t].ch;
  char chs[12]; if (ch == 1) snprintf(chs, sizeof chs, "MONO"); else if (ch == 2) snprintf(chs, sizeof chs, "STEREO"); else if (ch == 6) snprintf(chs, sizeof chs, "5.1"); else if (ch == 8) snprintf(chs, sizeof chs, "7.1"); else snprintf(chs, sizeof chs, "%d CH", ch);
  char lang[8]; snprintf(lang, sizeof lang, "%s", st->track[t].lang[0] ? st->track[t].lang : "?");
  for (char *q = lang; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
  snprintf(b, n, "%s %s %s", lang, cn, chs);
}
// LEFT/RIGHT while watching: the next audio track; says which in `label`, and remembers it for this channel.
void tv_audio_step(int dir, char *label, int n) {
  if (!s || !cur_ch) { snprintf(label, n, "NO CHANNEL"); return; }
  if (s->ntracks < 2) { snprintf(label, n, s->ntracks ? "ONE AUDIO TRACK" : "NO AUDIO"); return; }
  int cur = s->want_track >= 0 ? s->want_track : s->atrack;
  int t = (cur + dir + s->ntracks) % s->ntracks;
  s->want_track = t;
  char l[48]; track_label(s, t, l, sizeof l);
  snprintf(label, n, "AUDIO %d/%d: %s", t + 1, s->ntracks, l);
  audio_pref_set(cur_ch->key, s->track[t].lang, t);
}

// Stats for nerds: "LABEL|values" lines about the picture, sound, stream and device. Never an address or a login.
int tv_nerd(char out[][96], int max) {
  int n = 0;
  if (!nerd_seen) { nerd_seen = SDL_GetTicks(); SDL_DetachThread(SDL_CreateThread(wifi_thread, "wifi", NULL)); }
  else nerd_seen = SDL_GetTicks();
  #define LINE(...) do { if (n < max) snprintf(out[n++], 96, __VA_ARGS__); } while (0)
  if (!s || !cur_ch) { LINE("TV|off"); return n; }
  SDL_LockMutex(s->lock); int w = s->w, h = s->h, q = s->count, vp = s->vp_count; double sar = s->sar; SDL_UnlockMutex(s->lock);
  double aq = (double)SDL_GetQueuedAudioSize(audio) / (AUDIO_RATE * 4);
  LINE("CHANNEL|%.40s (%s)", cur_ch->name, cur_ch->source);
  LINE("VIDEO|%s %dx%d%s  %.2f fps  %s", s->vcodec[0] ? s->vcodec : "-", w, h, sar != 1.0 && sar > 0 ? " (non-square)" : "", s->vfps, s->hw ? "HARDWARE" : "software");
  LINE("FRAMES|decoded %.1f/s  shown %.1f/s  dropped %d", nerd_dec_fps, osd_fps, osd_drops);
  LINE("SYNC|A/V %+.0f ms  jumps %d  render %.0f/s", nerd_av * 1000, st_jumps_total, nerd_loop_fps);
  LINE("QUEUES|frames %d/%d  packets %d  audio %.0f ms", q, VQ, vp, aq * 1000);
  if (s->has_audio) LINE("AUDIO|%s %d Hz %d ch  track %d of %d", s->acodec, s->arate, s->ach, s->atrack + 1, s->ntracks); else LINE("AUDIO|none");
  if (s->ntracks > 1) {
    char all[96] = ""; size_t o = 0;
    for (int t = 0; t < s->ntracks && o < sizeof all - 20; t++) { char l[32]; track_label(s, t, l, sizeof l); o += snprintf(all + o, sizeof all - o, "%s%d %s", t ? "  " : "", t + 1, l); }
    LINE("TRACKS|%s", all);
  }
  if (s->variants > 1) LINE("STREAM|%.1f Mbps now  variant %d of %d (%.1f Mbps)", nerd_kbps / 1000, s->variant, s->variants, s->vbitrate / 1e6);
  else LINE("STREAM|%.1f Mbps now%s", nerd_kbps / 1000, s->vbitrate > 0 ? "" : "");
  LINE("HEALTH|retries %d  damaged frames %d%s", retries, s->damaged, s->heavy ? "  (above 720p: no deblock)" : "");
  char cpu[16] = "-", tmp[16] = "-", wifi[40] = "-";
  if (sys_cpu >= 0) snprintf(cpu, sizeof cpu, "%d%%", sys_cpu);
  if (sys_temp >= 0) snprintf(tmp, sizeof tmp, "%d C", sys_temp);
  if (sys_wifi) snprintf(wifi, sizeof wifi, "%d dBm, %d Mbit/s", sys_wifi, sys_wifi_mbit);
  LINE("DEVICE|CPU %s  %s  Wi-Fi %s", cpu, tmp, wifi);
  #undef LINE
  return n;
}

// A line of text centred on the screen, shortened with "..." when it is wider than the screen less a margin.
static void centre(const char *s, int h, int y, SDL_Color c) {
  char b[256]; snprintf(b, sizeof b, "%s", s);
  if (tt_text_width(h, b) > SCREEN_W - 80) {
    size_t n = strlen(b);
    while (n > 0) {
      do n--; while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80);   // back one whole character
      while (n > 0 && b[n - 1] == ' ') n--;
      memcpy(b + n, "...", 4);
      if (tt_text_width(h, b) <= SCREEN_W - 80) break;
    }
  }
  tt_draw((SCREEN_W - tt_text_width(h, b)) / 2, y, h, b, c);
}

// Watchdog, start buffer and A/V sync, then the picture (or the tuning screen, or NO SIGNAL) and the overlay.
void tv_render(void) {
  if (!cur_ch) return;
  nerd_tick();
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
    if (ijob->surface) { itex = SDL_CreateTextureFromSurface(ren, ijob->surface); iw = ijob->surface->w; ih = ijob->surface->h; ilogo = ijob->is_logo; SDL_FreeSurface(ijob->surface); }
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
    centre("NO SIGNAL", 64, SCREEN_H / 2 - 110, TT_PALETTE[TT_WHITE]);
    centre(cur_ch->name, 32, SCREEN_H / 2 - 20, TT_PALETTE[TT_YELLOW]);
    centre(fail_reason, 26, SCREEN_H / 2 + 30, TT_PALETTE[TT_WHITE]);
    centre("Up/down: another channel", 22, SCREEN_H / 2 + 90, TT_PALETTE[TT_CYAN]);
  } else if (!s->connected || !playing) {
    if (!set->plain_tuning) {   // plain: a black screen; the app draws the channel info and LOADING over it
      draw_tuning(ren, itex, iw, ih, ilogo, snow, SDL_GetTicks() - changed_at);
      if (!s->audio_started) { SDL_PauseAudioDevice(audio, 0); snow_audio(audio); }
    }
    if (retries) {   // reconnecting: say so over the snow
      char m[48]; snprintf(m, sizeof m, "SIGNAL LOST - RETRYING (%d/3)", retries);
      centre(m, 28, 40, TT_PALETTE[TT_YELLOW]);
    }
  } else {
    // Clock: the audio being heard (end of queued audio minus what's still queued). If the audio runs dry (or there
    // is none), the clock runs on the wall clock so the picture never freezes waiting for sound.
    SDL_LockMutex(s->lock);
    double clock;
    Uint64 nowc = SDL_GetPerformanceCounter();
    if (s->has_audio && queued > 0.02) {
      // The queued amount only drops a whole audio chunk (~21 ms) at a time, so the raw audio clock moves in steps,
      // which bunches up 50 fps frames and drops some. Run smoothly on the timer and ease towards the audio clock;
      // snap only when they are far apart (a new channel, a jump).
      double measured = s->audio_pts - queued;
      double predicted = wall_at ? wall_clock + (double)(nowc - wall_at) / SDL_GetPerformanceFrequency() : measured;
      double diff = measured - predicted;
      clock = diff > 0.08 || diff < -0.08 ? measured : predicted + diff * 0.05;
      wall_clock = clock; wall_at = nowc;
    }
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
        st_jumps++; st_jumps_total++; video_offset = clock - s->frames[s->head]->pts / 1e6;
        if (show) { av_frame_free(&show); st_dropped++; }
        show = s->frames[s->head]; s->head = (s->head + 1) % VQ; s->count--;
        break;
      }
      if (d > 0.01) break;
      nerd_av = nerd_av * 0.9 + d * 0.1;
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
