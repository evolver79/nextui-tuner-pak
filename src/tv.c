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
#include "tv.h"
#include "ui.h"
#include "hwdec.h"
#include "subs.h"
#include "cc.h"

#include "dot.h"
#define SCREEN_W DOT_W   // the device's screen: Brick 1024x768, Smart Pro 1280x720
#define SCREEN_H DOT_H
#define AUDIO_RATE 48000
#define VQ 6        // decoded video frames buffered ahead
#define VPKTS 512   // video packets buffered between demuxer and decoder
#define AUDIO_AHEAD 5.0   // seconds of decoded audio the demuxer keeps queued: a whole HLS segment (Pluto: 5 s), so a
                          // slow fetch of the next one is covered (1 s ran dry at every slow segment)
#define AUDIO_START 0.3   // seconds queued before sound (and the clock) starts
#define AUDIO_START_HEAVY 1.5   // ... for streams above 720p (big segments, Wi-Fi needs a head start)

#define DVB_PAGES 40   // DVB subtitle pages kept: they're decoded with the read-ahead (5 s), live ones come ~3 a second
typedef struct {
  // set by the decode thread, read by the main loop (under lock)
  SDL_mutex *lock;
  AVFrame *frames[VQ]; int head, count;   // ring of decoded, display-ready yuv420p frames
  int w, h; double sar;                   // video size and sample aspect ratio
  double audio_pts;                       // pts (s) of the end of the audio queued so far
  double video_read;                      // pts (s) of the last video packet read (how far the video runs ahead)
  double vshift;                          // the picture's timestamp shift (s, ts_join) as last read
  // when it changed, on the joined timeline: the shift for a moment shown (read ~5 s before it shows) is the one
  // from the latest change at or before it. (Subtitles beside the stream keep the stream's own timestamps.)
  struct { double from, shift; } shifts[8]; int nshifts;
  VttTrack vtt[8]; int nvtt; volatile int vtt_found;   // subtitles beside the stream, found in the background
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
  struct { int idx, ch, kept; char lang[8], codec[16]; } track[8]; int ntracks, atrack; volatile int want_track;   // kept: read all along
  // copied from the settings and the channel when the stream starts
  int max_h; char langs[256], pref_lang[8]; int pref_pos;
  volatile long long bytes; volatile int damaged, decoded;
  // closed captions (CEA-608 in the video's SEI): seen at all; wanted (decode them); what they said, when
  volatile int has_cc, cc_on;
  // DVB subtitles (bitmaps in the stream): the tracks (found when it opens); the one wanted (-1 none); the pages
  // decoded, with when they show (a ring; each holds its bitmaps, ARGB, on the decoder's canvas)
  struct { int idx; char lang[8]; } dvb[6]; int ndvb; volatile int opened, want_dvb;
  struct DvbPage { double at, end; int cw, ch, n, id; struct { int x, y, w, h; uint32_t *px; } r[8]; } pages[DVB_PAGES]; int npages, page_head;
  struct { double at, end; char text[256]; } cc[48]; int ncc, cc_head;
} Stream;

// Stats, logged every 2 s: frames decoded/shown/dropped, render loop rate, upload/present time, queue depths.
static int st_decoded, st_shown, st_dropped, st_loops, st_jumps, st_jumps_total;
static double st_upload_ms, st_present_ms;

static double frame_pts(AVFrame *f, AVStream *st) {
  int64_t p = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  return p == AV_NOPTS_VALUE ? 0 : p * av_q2d(st->time_base);
}

static int interrupt_cb(void *opaque) { return ((Stream *)opaque)->stop; }

// sarc: with the video engine, which only gives square pixels, a small software decoder that SD streams' key frames
// also go through, just for their pixel shape (720x576 and 720x480 carry 4:3 or 16:9 in it, and channels switch).
typedef struct { Stream *s; AVCodecContext *vc; AVStream *vst; HwDec *hw; AVCodecContext *sarc; double sar; } VideoJob;
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
      if (j->sarc && (pkt->flags & AV_PKT_FLAG_KEY)) {
        if (avcodec_send_packet(j->sarc, pkt) == 0) while (avcodec_receive_frame(j->sarc, fr) == 0) av_frame_unref(fr);
        AVRational q = j->sarc->sample_aspect_ratio;
        if (q.num > 0 && q.den > 0 && fabs(av_q2d(q) - j->sar) > 0.001) { j->sar = av_q2d(q); fprintf(stderr, "video: pixel shape %d:%d\n", q.num, q.den); }
        avcodec_flush_buffers(j->sarc);
      }
      for (int spins = 0; (r = hwdec_send(j->hw, pkt)) == AVERROR(EAGAIN) && !s->stop && spins < 500; spins++) {
        AVFrame *f; int n = 0;
        while (!stopped && (f = hwdec_receive(j->hw, &err))) { n++; if (ring_put(s, f, j->sar, 0) < 0) stopped = 1; }
        if (!n) SDL_Delay(2);
      }
      AVFrame *f;
      while (!stopped && r >= 0 && (f = hwdec_receive(j->hw, &err))) if (ring_put(s, f, j->sar, 0) < 0) stopped = 1;
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
  if (j->sarc) avcodec_free_context(&j->sarc);
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

// Timestamp jumps: FAST channels splice in ads (#EXT-X-DISCONTINUITY) whose timestamps restart. Packets are joined
// onto one timeline here, before decoding, with the same shift for the sound and the picture, so each piece keeps its
// own lip sync. The first of the two to cross a jump sets the shift; the other takes the same one when it crosses
// (its new timestamps land near where the first one's did). A packet that fits the other's timeline is a gap, not
// a jump (the sound missing for a while): it keeps its timestamps.
#define TS_JUMP 1000000   // µs off the expected timestamp: a jump
#define TS_NEAR 2500000   // µs: the same jump (sound and picture are muxed up to a second or two apart), or a gap
typedef struct { int64_t off[2], next[2], jump_raw, jump_off; } TsJoin;   // [0] video, [1] audio; µs
static void ts_join(TsJoin *j, AVPacket *pkt, AVStream *st, int k) {
  int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
  if (ts == AV_NOPTS_VALUE) return;
  int64_t raw = av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q), now = raw + j->off[k];
  if (j->next[k] != AV_NOPTS_VALUE && llabs(now - j->next[k]) > TS_JUMP) {
    const char *how;
    if (j->jump_raw != AV_NOPTS_VALUE && llabs(raw - j->jump_raw) < TS_NEAR && j->off[k] != j->jump_off) { j->off[k] = j->jump_off; how = "follows"; }
    else if (j->next[!k] != AV_NOPTS_VALUE && llabs(now - j->next[!k]) < TS_NEAR) how = "gap, kept";
    else { j->off[k] = j->next[k] - raw; j->jump_off = j->off[k]; j->jump_raw = raw; how = "joined"; }
    fprintf(stderr, "timestamps: %s jumped %+.2f s (%s, shift %+.2f s)\n", k ? "audio" : "video", (now - j->next[k]) / 1e6, how, j->off[k] / 1e6);
    now = raw + j->off[k];
  }
  if (j->off[k]) {
    int64_t d = av_rescale_q(j->off[k], AV_TIME_BASE_Q, st->time_base);
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += d;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += d;
  }
  j->next[k] = now + (pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : 0);
}

// Audio packets kept per track, the last AUDIO_KEEP seconds: switching track decodes the new one from where you are.
#define AUDIO_KEEP 8.0
#define AKEEP 640
typedef struct { AVPacket *p[AKEEP]; int head, n; } AudioKeep;
static void keep_push(AudioKeep *k, const AVPacket *pkt, AVRational tb) {
  if (k->n == AKEEP) { av_packet_free(&k->p[k->head]); k->head = (k->head + 1) % AKEEP; k->n--; }
  k->p[(k->head + k->n) % AKEEP] = av_packet_clone(pkt); k->n++;
  while (k->n > 1) {   // older than AUDIO_KEEP behind the newest: gone
    AVPacket *o = k->p[k->head];
    if (o->pts == AV_NOPTS_VALUE || pkt->pts == AV_NOPTS_VALUE || (pkt->pts - o->pts) * av_q2d(tb) < AUDIO_KEEP) break;
    av_packet_free(&k->p[k->head]); k->head = (k->head + 1) % AKEEP; k->n--;
  }
}
static void keep_free(AudioKeep *k) { while (k->n) { av_packet_free(&k->p[k->head]); k->head = (k->head + 1) % AKEEP; k->n--; } }

// One audio packet: decoded, made 48 kHz stereo, into SDL's queue; audio_pts follows.
typedef struct { AVCodecContext *ac; SwrContext *swr; AVStream *st; AVFrame *fr; uint8_t *buf; int size; } AudioDec;
static void decode_audio(Stream *s, AudioDec *a, const AVPacket *pkt) {
  if (!a->ac || avcodec_send_packet(a->ac, pkt) != 0) return;
  while (avcodec_receive_frame(a->ac, a->fr) == 0) {
    AVFrame *fr = a->fr;
    if (!a->swr) {
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      swr_alloc_set_opts2(&a->swr, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE, &fr->ch_layout, fr->format, fr->sample_rate, 0, NULL);
      swr_init(a->swr);
    }
    int n = swr_get_out_samples(a->swr, fr->nb_samples);
    if (n * 4 > a->size) { a->size = n * 4; a->buf = realloc(a->buf, a->size); }
    int got = swr_convert(a->swr, &a->buf, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
    if (got > 0) {
      if (!s->audio_started) { SDL_ClearQueuedAudio(s->audio); s->audio_started = 1; }  // drop the snow hiss
      SDL_QueueAudio(s->audio, a->buf, got * 4);
      SDL_LockMutex(s->lock);
      s->audio_pts = frame_pts(fr, a->st) + (double)fr->nb_samples / fr->sample_rate;
      SDL_UnlockMutex(s->lock);
    }
  }
}

// DVB subtitle pages: decoded in the demuxer (the bitmaps made ARGB), kept with when they show.
static SDL_atomic_t dvb_ids;   // page ids, unique across streams (the screen keeps its textures by id)
static void dvb_page_free(struct DvbPage *pg) { for (int i = 0; i < pg->n; i++) free(pg->r[i].px); pg->n = 0; }
static void dvb_decode(Stream *s, AVCodecContext *dc, AVPacket *pkt) {
  AVSubtitle sub; int got = 0;
  if (avcodec_decode_subtitle2(dc, &sub, &got, pkt) < 0 || !got) return;
  struct DvbPage pg = {0};
  pg.at = (sub.pts != AV_NOPTS_VALUE ? sub.pts / 1e6 : pkt->pts * av_q2d(dc->pkt_timebase)) + sub.start_display_time / 1000.0;
  pg.end = sub.end_display_time && sub.end_display_time != UINT32_MAX ? pg.at + (sub.end_display_time - sub.start_display_time) / 1000.0 : -1;
  pg.cw = dc->width > 0 ? dc->width : 720; pg.ch = dc->height > 0 ? dc->height : 576;
  for (unsigned r = 0; r < sub.num_rects && pg.n < 8; r++) {
    AVSubtitleRect *sr = sub.rects[r];
    if (sr->type != SUBTITLE_BITMAP || sr->w <= 0 || sr->h <= 0 || !sr->data[0] || !sr->data[1]) continue;
    uint32_t *px = malloc((size_t)sr->w * sr->h * 4), *pal = (uint32_t *)sr->data[1];
    if (!px) continue;
    for (int y = 0; y < sr->h; y++) for (int x = 0; x < sr->w; x++) px[y * sr->w + x] = pal[sr->data[0][y * sr->linesize[0] + x]];
    pg.r[pg.n].x = sr->x; pg.r[pg.n].y = sr->y; pg.r[pg.n].w = sr->w; pg.r[pg.n].h = sr->h; pg.r[pg.n].px = px; pg.n++;
  }
  avsubtitle_free(&sub);
  SDL_LockMutex(s->lock);
  int i = (s->page_head + s->npages) % DVB_PAGES;
  if (s->npages == DVB_PAGES) { dvb_page_free(&s->pages[s->page_head]); s->page_head = (s->page_head + 1) % DVB_PAGES; } else s->npages++;
  pg.id = SDL_AtomicAdd(&dvb_ids, 1) + 1;
  s->pages[i] = pg;
  SDL_UnlockMutex(s->lock);
}

// Closed captions (cc.c): what they say goes into the Stream, with when.
static void cc_emit(void *ud, double at, double end, const char *text) {
  Stream *s = ud;
  SDL_LockMutex(s->lock);
  int i = (s->cc_head + s->ncc) % 48;
  if (s->ncc == 48) s->cc_head = (s->cc_head + 1) % 48; else s->ncc++;
  s->cc[i].at = at; s->cc[i].end = end; snprintf(s->cc[i].text, sizeof s->cc[i].text, "%s", text);
  SDL_UnlockMutex(s->lock);
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
  // The picture and every audio track that goes with it (the others kept a few seconds back: switching picks up
  // where you are); the rest left out.
  // In HLS, only the tracks in the picture's own segments (same variant_bitrate): a track in a playlist of its own
  // would be a download of its own, so it's read only while it plays (switching to it then starts where it's read).
  {
    int hls = fmt->iformat && !strcmp(fmt->iformat->name, "hls");
    AVDictionaryEntry *vb = av_dict_get(fmt->streams[vi]->metadata, "variant_bitrate", NULL, 0);
    for (int k = 0; k < s->ntracks; k++) {
      AVDictionaryEntry *ab = av_dict_get(fmt->streams[s->track[k].idx]->metadata, "variant_bitrate", NULL, 0);
      s->track[k].kept = !hls || (vb && ab && !strcmp(vb->value, ab->value));
    }
  }
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    int keep = (int)i == vi || (int)i == ai;
    for (int k = 0; k < s->ntracks; k++) if (s->track[k].idx == (int)i && s->track[k].kept) keep = 1;
    fmt->streams[i]->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }

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
  {   // DVB subtitles that go with the picture
    AVProgram *prog = av_find_program_from_stream(fmt, NULL, vi);
    unsigned count = prog ? prog->nb_stream_indexes : fmt->nb_streams;
    for (unsigned k = 0; k < count && s->ndvb < 6; k++) {
      int i = prog ? (int)prog->stream_index[k] : (int)k;
      if (fmt->streams[i]->codecpar->codec_id != AV_CODEC_ID_DVB_SUBTITLE) continue;
      AVDictionaryEntry *e = av_dict_get(fmt->streams[i]->metadata, "language", NULL, 0);
      s->dvb[s->ndvb].idx = i; snprintf(s->dvb[s->ndvb].lang, sizeof s->dvb[s->ndvb].lang, "%.3s", e ? e->value : "");
      s->ndvb++;
    }
    if (s->ndvb) fprintf(stderr, "subtitles: %d DVB track%s in the stream\n", s->ndvb, s->ndvb > 1 ? "s" : "");
    s->opened = 1;
  }
  s->has_audio = ast != NULL;
  if (ast) { snprintf(s->acodec, sizeof s->acodec, "%s", avcodec_get_name(ast->codecpar->codec_id)); s->arate = ast->codecpar->sample_rate; s->ach = ast->codecpar->ch_layout.nb_channels; }

  AVRational sar0 = av_guess_sample_aspect_ratio(fmt, vst, NULL);
  VideoJob job = { s, vc, vst, NULL, NULL, sar0.num > 0 && sar0.den > 0 ? av_q2d(sar0) : 1.0 };
  if (use_hw && (job.hw = hwdec_open(vst->codecpar, vst->time_base, s->vfps))) {
    s->hw = 1;
    if (vst->codecpar->height <= 576 && (job.sarc = avcodec_alloc_context3(vdec))) {
      avcodec_parameters_to_context(job.sarc, vst->codecpar);
      job.sarc->thread_count = 1; job.sarc->flags |= AV_CODEC_FLAG_LOW_DELAY; job.sarc->skip_loop_filter = AVDISCARD_ALL;
      if (avcodec_open2(job.sarc, vdec, NULL) < 0) avcodec_free_context(&job.sarc);
    }
    if (job.sar != 1.0) fprintf(stderr, "video: pixel shape %d:%d\n", sar0.num, sar0.den);
  }
  fprintf(stderr, "video: %s %dx%d, %s decoding\n", s->vcodec, vst->codecpar->width, vst->codecpar->height, s->hw ? "hardware" : "software");
  SDL_Thread *vt = SDL_CreateThread(video_thread, "video", &job);

  AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
  AudioDec ad = { ac, swr, ast, fr, NULL, 0 };
  AudioKeep *keep = calloc(s->ntracks > 0 ? s->ntracks : 1, sizeof *keep);
  TsJoin tj = { {0, 0}, {AV_NOPTS_VALUE, AV_NOPTS_VALUE}, AV_NOPTS_VALUE, 0 };
  CcState *cc = cc_open();
  int dvb_now = -1; AVCodecContext *dvbc = NULL;   // the DVB track being read and its decoder
  int cc_codec = vst->codecpar->codec_id == AV_CODEC_ID_H264 ? 1 : vst->codecpar->codec_id == AV_CODEC_ID_HEVC ? 2 : 0;
  while (!s->stop) {
    int want = s->want_track;
    if (want >= 0 && want < s->ntracks && want != s->atrack) {   // another audio track, the picture goes on
      s->want_track = -1;
      // where you are: the new track's kept packets from there go into the queue, so nothing is skipped
      double heard = s->audio_pts - (double)SDL_GetQueuedAudioSize(s->audio) / (AUDIO_RATE * 4);
      if (ad.ac) avcodec_free_context(&ad.ac);
      if (ad.swr) swr_free(&ad.swr);
      int old = s->atrack;
      if (old >= 0 && old < s->ntracks && !s->track[old].kept) fmt->streams[s->track[old].idx]->discard = AVDISCARD_ALL;
      ai = s->track[want].idx; s->atrack = want;
      fmt->streams[ai]->discard = AVDISCARD_DEFAULT;
      ast = ad.st = fmt->streams[ai];
      if (!(ad.ac = open_audio(ast))) ast = ad.st = NULL;
      snprintf(s->acodec, sizeof s->acodec, "%s", s->track[want].codec);
      s->arate = ast ? ast->codecpar->sample_rate : 0; s->ach = s->track[want].ch;
      SDL_ClearQueuedAudio(s->audio);
      tj.next[1] = AV_NOPTS_VALUE;      // same timeline, same shift: only no last timestamp to compare with
      int from = 0; AudioKeep *k = &keep[want];
      for (int i = 0; i < k->n; i++) {
        AVPacket *q = k->p[(k->head + i) % AKEEP];
        if (q->pts != AV_NOPTS_VALUE && q->pts * av_q2d(ast ? ast->time_base : (AVRational){1, 1}) < heard - 0.03) from = i + 1;
      }
      for (int i = from; ast && i < k->n; i++) decode_audio(s, &ad, k->p[(k->head + i) % AKEEP]);
      fprintf(stderr, "audio: track %d, %d kept packets from %.2f s\n", want + 1, k->n - from, heard);
      s->has_audio = ast != NULL;
    } else if (want >= 0) s->want_track = -1;
    if (s->want_dvb != dvb_now) {   // DVB subtitles on, off or another language: read that track only
      if (dvb_now >= 0) fmt->streams[s->dvb[dvb_now].idx]->discard = AVDISCARD_ALL;
      if (dvbc) avcodec_free_context(&dvbc);
      dvb_now = s->want_dvb;
      SDL_LockMutex(s->lock); while (s->npages) { dvb_page_free(&s->pages[s->page_head]); s->page_head = (s->page_head + 1) % DVB_PAGES; s->npages--; } SDL_UnlockMutex(s->lock);
      if (dvb_now >= 0 && dvb_now < s->ndvb) {
        AVStream *st = fmt->streams[s->dvb[dvb_now].idx];
        const AVCodec *d = avcodec_find_decoder(AV_CODEC_ID_DVB_SUBTITLE);
        if (d && (dvbc = avcodec_alloc_context3(d))) {
          avcodec_parameters_to_context(dvbc, st->codecpar); dvbc->pkt_timebase = st->time_base;
          if (avcodec_open2(dvbc, d, NULL) < 0) avcodec_free_context(&dvbc);
        }
        st->discard = AVDISCARD_DEFAULT;
      } else dvb_now = -1;
    }
    // Enough read ahead? (audio well queued and video packets waiting, or the packet queue full)
    SDL_LockMutex(s->lock); int vp = s->vp_count; SDL_UnlockMutex(s->lock);
    double aq = (double)SDL_GetQueuedAudioSize(s->audio) / (AUDIO_RATE * 4);
    if (vp >= VPKTS - 1 || ((!ast || aq >= AUDIO_AHEAD) && vp >= 30)) { SDL_Delay(5); continue; }
    if (av_read_frame(fmt, pkt) < 0) { if (!s->connected) { snprintf(s->error, sizeof s->error, "no signal"); s->failed = 1; } break; }
    if (pkt->stream_index == vi || pkt->stream_index == ai) s->bytes += pkt->size;
    int ti = -1;   // which audio track
    for (int k = 0; k < s->ntracks; k++) if (s->track[k].idx == pkt->stream_index) ti = k;
    if (pkt->stream_index == vi) {
      ts_join(&tj, pkt, vst, 0);
      if (tj.off[0] / 1e6 != s->vshift && pkt->pts != AV_NOPTS_VALUE) {   // a jump joined: from here on, this shift
        SDL_LockMutex(s->lock);
        if (s->nshifts == 8) { memmove(s->shifts, s->shifts + 1, 7 * sizeof *s->shifts); s->nshifts--; }
        s->shifts[s->nshifts].from = pkt->pts * av_q2d(vst->time_base); s->shifts[s->nshifts].shift = tj.off[0] / 1e6; s->nshifts++;
        SDL_UnlockMutex(s->lock);
      }
      s->vshift = tj.off[0] / 1e6; if (cc_codec && cc_packet(cc, pkt, vst->time_base, cc_codec == 2, s->cc_on, cc_emit, s) && !s->has_cc) { s->has_cc = 1; fprintf(stderr, "subtitles: closed captions in the picture\n"); } }
    else if (ast && pkt->stream_index == ai) ts_join(&tj, pkt, ast, 1);
    else if (ti >= 0 && tj.off[1]) {   // the other tracks: the playing one's shift (same timeline)
      int64_t d = av_rescale_q(tj.off[1], AV_TIME_BASE_Q, fmt->streams[pkt->stream_index]->time_base);
      if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += d;
      if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += d;
    }
    if (ti >= 0) keep_push(&keep[ti], pkt, fmt->streams[pkt->stream_index]->time_base);
    if (pkt->stream_index == vi) {
      SDL_LockMutex(s->lock);
      int64_t vts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
      if (vts != AV_NOPTS_VALUE) s->video_read = vts * av_q2d(vst->time_base);
      s->vpkts[(s->vp_head + s->vp_count) % VPKTS] = av_packet_clone(pkt); s->vp_count++;
      SDL_CondSignal(s->vp_cond);
      SDL_UnlockMutex(s->lock);
    } else if (ast && pkt->stream_index == ai) decode_audio(s, &ad, pkt);
    else if (dvbc && dvb_now >= 0 && pkt->stream_index == s->dvb[dvb_now].idx) {
      if (tj.off[0]) {   // the picture's shift (same timeline)
        int64_t d = av_rescale_q(tj.off[0], AV_TIME_BASE_Q, fmt->streams[pkt->stream_index]->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += d;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += d;
      }
      dvb_decode(s, dvbc, pkt);
    }
    av_packet_unref(pkt);
  }
  cc_close(cc);
  if (dvbc) avcodec_free_context(&dvbc);
  for (int k = 0; k < s->ntracks; k++) keep_free(&keep[k]);
  free(keep);
  ac = ad.ac; swr = ad.swr;
  SDL_LockMutex(s->lock); s->eof = 1; SDL_CondSignal(s->vp_cond); SDL_UnlockMutex(s->lock);
  SDL_WaitThread(vt, NULL);
  SDL_LockMutex(s->lock);
  while (s->vp_count) { av_packet_free(&s->vpkts[s->vp_head]); s->vp_head = (s->vp_head + 1) % VPKTS; s->vp_count--; }
  SDL_UnlockMutex(s->lock);
  av_packet_free(&pkt); av_frame_free(&fr); free(ad.buf);
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
    ui_searching();
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

// Looks for subtitles beside an HLS stream (a second fetch of its master playlist), without holding up the picture.
// The Stream is never freed (see stop_stream), so this may finish after it stopped.
static int vtt_find_thread(void *arg) {
  Stream *st = arg; VttTrack found[8];
  int n = vtt_find(st->url, st->user_agent, found, 8);
  SDL_LockMutex(st->lock); memcpy(st->vtt, found, sizeof found); st->nvtt = n; st->vtt_found = 1; SDL_UnlockMutex(st->lock);
  if (n) fprintf(stderr, "subtitles: %d track%s beside the stream\n", n, n > 1 ? "s" : "");
  return 0;
}
static Stream *start_stream(const Channel *c, SDL_AudioDeviceID audio) {
  Stream *s = calloc(1, sizeof *s);
  s->lock = SDL_CreateMutex(); s->vp_cond = SDL_CreateCond(); s->url = c->url; s->user_agent = c->user_agent; s->audio = audio; s->sar = 1;
  s->want_track = -1; s->want_dvb = -1; s->max_h = set ? set->max_height : 720;
  if (set) snprintf(s->langs, sizeof s->langs, "%s", set->languages);
  s->pref_pos = audio_pref_get(c->key, s->pref_lang, sizeof s->pref_lang);
  SDL_ClearQueuedAudio(audio);
  SDL_DetachThread(SDL_CreateThread(decode_thread, "decode", s));
  if (strstr(c->url, "m3u")) SDL_DetachThread(SDL_CreateThread(vtt_find_thread, "vttfind", s));
  else s->vtt_found = 1;
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
// Subtitles on (WebVTT beside the stream): the track (-1 none), its follower; whether the channel's choice was applied;
// the clock of the last frame (subtitles are timed on it).
#define SUB_CC 100    // sub_on: the closed captions
#define SUB_DVB 200   // sub_on: SUB_DVB + the DVB track
static int sub_on = -1, sub_checked; static Vtt *vtt_on; static double last_clock;
static void subs_off(void) { vtt_stop(vtt_on); vtt_on = NULL; if (s) { s->cc_on = 0; s->want_dvb = -1; } sub_on = -1; sub_checked = 0; }
static const char *fail_reason = "";   // why the channel gave up, in plain words
static Uint32 started_at, changed_at, shown_at;   // shown_at: ticks of the last new picture (tv_buffering)
static double wall_clock, video_offset; static Uint64 wall_at;
static int rebuf;   // the sound ran dry while playing: the clock (and the picture) wait until it's queued again
// How much sound to wait for before going on: AUDIO_START, doubled each time it runs dry again within 10 s (up to
// 4 s), so a stream that comes barely fast enough plays in longer stretches instead of stuttering.
static double rebuf_need; static Uint32 rebuf_last;
static int osd_frames, osd_drops; static Uint32 osd_at; static double osd_fps;
static double nerd_av, nerd_dec_fps, nerd_kbps, nerd_loop_fps; static int nerd_loops, nerd_dec_last; static long long nerd_bytes_last;

// --- Crop black borders (Settings, L2/R2 while watching): a 4:3 film pillarboxed in a 16:9 feed gets black on all
// four sides of the Brick's 4:3 screen. A few times a second the shown frame's luma is sampled for black bars; a
// crop that holds steady for a while is applied, one that shows less bar is applied at once, so a dark scene can't
// zoom the picture for long and an ad in full 16:9 is never cut. Bars are cropped evenly on both sides (the
// smaller of the two), so a logo or ticker in one bar never makes the picture lopsided.
#define CROP_THR 32          // luma at or below this is black (limited-range black is 16, plus noise)
#define CROP_SAMPLES 48      // samples per row or column tested
#define CROP_LIT 3           // ... of which this many may be bright (a channel logo or ticker in the bar)
#define CROP_EVERY_MS 250
#define CROP_GROW 16         // samples (4 s) a bigger crop must hold before it is applied
#define CROP_SHRINK 2        // samples (0.5 s) for a smaller one
typedef struct { int l, r, t, b; } Crop;
static Crop crop_on, crop_cand; static int crop_n, crop_w, crop_h; static Uint32 crop_at;

static int crop_row_black(const uint8_t *y, int ls, int row, int x0, int x1) {
  int lit = 0;
  for (int i = 0; i < CROP_SAMPLES; i++) if (y[row * ls + x0 + (x1 - x0 - 1) * i / (CROP_SAMPLES - 1)] > CROP_THR && ++lit > CROP_LIT) return 0;
  return 1;
}
static int crop_col_black(const uint8_t *y, int ls, int col, int y0, int y1) {
  int lit = 0;
  for (int i = 0; i < CROP_SAMPLES; i++) if (y[(y0 + (y1 - y0 - 1) * i / (CROP_SAMPLES - 1)) * ls + col] > CROP_THR && ++lit > CROP_LIT) return 0;
  return 1;
}
// The black bars of a yuv420p frame, or -1 when the frame is (nearly) all black and says nothing.
static int crop_detect(const AVFrame *f, Crop *c) {
  const uint8_t *y = f->data[0]; int ls = f->linesize[0], w = f->width, h = f->height;
  if (w < 64 || h < 64) return -1;
  int lit = 0;   // a black or near-black frame (a fade, a night scene) gives no answer
  for (int gy = 1; gy < 16; gy++) for (int gx = 1; gx < 16; gx++) lit += y[(h * gy / 16) * ls + w * gx / 16] > CROP_THR;
  if (lit < 8) return -1;
  int t = 0, b = 0, l = 0, r = 0;
  while (t < h / 3 && crop_row_black(y, ls, t, 0, w)) t++;
  while (b < h / 3 && crop_row_black(y, ls, h - 1 - b, 0, w)) b++;
  while (l < w / 3 && crop_col_black(y, ls, l, t, h - b)) l++;
  while (r < w / 3 && crop_col_black(y, ls, w - 1 - r, t, h - b)) r++;
  int v = t < b ? t : b, hz = l < r ? l : r;
  // Real bars are thinner than a fifth (4:3 in 16:9: 12.5 % a side; 2.39:1 in 16:9: 13 % top and bottom). More is a
  // dark frame with something in the middle (a studio or channel logo, titles): it says nothing about bars.
  if (v > h / 5 || hz > w / 5) return -1;
  if (v < h / 50) v = 0;     // a thin edge isn't worth a zoom
  if (hz < w / 50) hz = 0;
  c->t = c->b = v & ~1; c->l = c->r = hz & ~1;   // even: the chroma planes are half size
  return 0;
}
static int crop_close(int a, int b, int dim) { int tol = dim / 100 > 4 ? dim / 100 : 4; return abs(a - b) <= tol; }
static void crop_reset(void) { memset(&crop_on, 0, sizeof crop_on); memset(&crop_cand, 0, sizeof crop_cand); crop_n = crop_w = crop_h = 0; }
// Called with each frame about to be shown.
static void crop_update(const AVFrame *f) {
  if (!set->autocrop) { crop_reset(); return; }
  if (f->width != crop_w || f->height != crop_h) { crop_reset(); crop_w = f->width; crop_h = f->height; }   // another variant
  Uint32 now = SDL_GetTicks();
  if (now - crop_at < CROP_EVERY_MS) return;
  crop_at = now;
  Crop d;
  if (crop_detect(f, &d) < 0) return;
  int w = f->width, h = f->height;
  if (crop_close(d.l, crop_cand.l, w) && crop_close(d.t, crop_cand.t, h)) crop_n++;
  else { crop_cand = d; crop_n = 1; }
  int less = (crop_cand.l < crop_on.l && !crop_close(crop_cand.l, crop_on.l, w)) ||   // picture showing where the crop cuts
             (crop_cand.t < crop_on.t && !crop_close(crop_cand.t, crop_on.t, h));
  int same = crop_close(crop_cand.l, crop_on.l, w) && crop_close(crop_cand.t, crop_on.t, h);
  if (less ? crop_n >= CROP_SHRINK : !same && crop_n >= CROP_GROW) {
    crop_on = crop_cand;
    fprintf(stderr, "crop: %dx%d -> %dx%d\n", w, h, w - crop_on.l - crop_on.r, h - crop_on.t - crop_on.b);
  }
}

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
  retries = no_signal = playing = rebuf = 0; wall_at = 0; video_offset = 0; shown_at = 0; crop_reset(); subs_off();
  started_at = changed_at = SDL_GetTicks();
  if (!c) { if (video) { SDL_DestroyTexture(video); video = NULL; } return; }
  s = start_stream(c, audio);
  ijob = fetch_image(c);
  fprintf(stderr, "tune: %s (%s)\n", c->name, c->source);   // not the URL: yours can hold a login
}

Channel *tv_channel(void) { return cur_ch; }
int tv_state(void) { return !cur_ch ? TV_OFF : no_signal ? TV_NO_SIGNAL : playing ? TV_PLAYING : TV_TUNING; }
Uint32 tv_tuned_ms(void) { return SDL_GetTicks() - changed_at; }
int tv_retries(void) { return cur_ch && !no_signal ? retries : 0; }
// Playing, but the stream has run dry: no new picture for a second and no sound left to play.
int tv_buffering(void) {
  if (!cur_ch || !s || no_signal || !playing || !shown_at || SDL_GetTicks() - shown_at < 1000) return 0;
  SDL_LockMutex(s->lock); int frames = s->count; SDL_UnlockMutex(s->lock);
  return rebuf || (!frames && (!s->has_audio || SDL_GetQueuedAudioSize(audio) < AUDIO_RATE * 4 / 20));
}
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
// --- the picker (LEFT/RIGHT while watching): audio tracks and subtitles ----------------------------------------
int tv_audio_tracks(char labels[][48], int max, int *cur) {
  *cur = -1;
  if (!s) return 0;
  int n = s->ntracks < max ? s->ntracks : max;
  for (int t = 0; t < n; t++) track_label(s, t, labels[t], 48);
  *cur = s->want_track >= 0 ? s->want_track : s->atrack;
  return n;
}
void tv_audio_set(int t) {
  if (!s || !cur_ch || t < 0 || t >= s->ntracks) return;
  s->want_track = t;
  audio_pref_set(cur_ch->key, s->track[t].lang, t);
}
// The subtitle choices, in the picker's order: the WebVTT tracks, the closed captions (once the picture has shown
// it carries some), the DVB tracks. Each as its sub_on code.
static int sub_codes(int *code, int max) {
  int n = 0;
  for (int t = 0; t < s->nvtt && n < max; t++) code[n++] = t;
  if ((s->has_cc || sub_on == SUB_CC) && n < max) code[n++] = SUB_CC;
  for (int t = 0; t < s->ndvb && n < max; t++) code[n++] = SUB_DVB + t;
  return n;
}
int tv_sub_tracks(char labels[][48], int max, int *cur) {
  *cur = -1;
  if (!s) return 0;
  SDL_LockMutex(s->lock);
  int code[24], n = sub_codes(code, max < 24 ? max : 24);
  for (int i = 0; i < n; i++) {
    int c = code[i]; char lang[8] = "";
    if (c == sub_on) *cur = i;
    if (c == SUB_CC) { snprintf(labels[i], 48, "Closed captions (CC1)"); continue; }
    const char *l = c >= SUB_DVB ? s->dvb[c - SUB_DVB].lang : s->vtt[c].lang;
    snprintf(lang, sizeof lang, "%s", l); for (char *q = lang; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
    if (c >= SUB_DVB) snprintf(labels[i], 48, "%s · DVB", lang[0] ? lang : "Subtitles");
    else if (lang[0] && strcasecmp(s->vtt[c].name, s->vtt[c].lang)) snprintf(labels[i], 48, "%s · %s", s->vtt[c].name, lang);
    else snprintf(labels[i], 48, "%s", lang[0] ? lang : s->vtt[c].name);
  }
  SDL_UnlockMutex(s->lock);
  return n;
}
// Turns on the subtitles with that sub_on code (-1: off), remembering it for the channel when `remember`.
static void sub_turn_on(int c, int remember) {
  vtt_stop(vtt_on); vtt_on = NULL; s->cc_on = 0; s->want_dvb = -1; sub_on = -1;
  SDL_LockMutex(s->lock); s->ncc = 0; SDL_UnlockMutex(s->lock);
  char pref[48] = "";
  if (c >= 0 && c < s->nvtt) { vtt_on = vtt_start(&s->vtt[c], s->user_agent); sub_on = c; snprintf(pref, sizeof pref, "%s", s->vtt[c].lang[0] ? s->vtt[c].lang : s->vtt[c].name); }
  else if (c == SUB_CC) { s->cc_on = 1; sub_on = SUB_CC; snprintf(pref, sizeof pref, "CC1"); }
  else if (c >= SUB_DVB && c - SUB_DVB < s->ndvb) { s->want_dvb = c - SUB_DVB; sub_on = c; snprintf(pref, sizeof pref, "dvb:%s", s->dvb[c - SUB_DVB].lang); }
  if (remember) sub_pref_set(cur_ch->key, pref);
}
void tv_sub_set(int t) {
  if (!s || !cur_ch) return;
  int code[24], n = sub_codes(code, 24);
  sub_turn_on(t >= 0 && t < n ? code[t] : -1, 1);
  sub_checked = 1;
}
// The subtitle on now ("" none). Also applies the channel's remembered choice once its subtitles are found.
void tv_subtitle(char *out, size_t n) {
  out[0] = 0;
  if (!s || !cur_ch) return;
  if (!sub_checked && s->vtt_found && s->opened) {   // the channel's choice, once its subtitles are known
    sub_checked = 1;
    char want[48];
    if (sub_pref_get(cur_ch->key, want, sizeof want) && want[0]) {
      if (!strcmp(want, "CC1")) sub_turn_on(SUB_CC, 0);   // (shown once the picture brings some)
      else if (!strncmp(want, "dvb:", 4)) { for (int t = 0; t < s->ndvb; t++) if (!strcmp(s->dvb[t].lang, want + 4) || lang_match(s->dvb[t].lang, want + 4)) { sub_turn_on(SUB_DVB + t, 0); break; } }
      else for (int t = 0; t < s->nvtt; t++)
        if (lang_match(s->vtt[t].lang, want) || !strcmp(s->vtt[t].name, want)) { sub_turn_on(t, 0); break; }
    }
  }
  if (!playing || no_signal) return;
  if (vtt_on) {   // on the stream's own clock: less the shift in effect for what's shown
    double t = last_clock - video_offset, shift = 0;
    SDL_LockMutex(s->lock);
    for (int i = 0; i < s->nshifts; i++) if (s->shifts[i].from <= t) shift = s->shifts[i].shift;
    SDL_UnlockMutex(s->lock);
    vtt_text_at(vtt_on, t - shift, out, n);
  }
  else if (sub_on == SUB_CC) {   // the last caption from before now (cleared captions come as an empty one), 15 s at most
    double now = last_clock - video_offset; int best = -1;
    SDL_LockMutex(s->lock);
    for (int i = 0; i < s->ncc; i++) { int k = (s->cc_head + i) % 48; if (s->cc[k].at <= now && (best < 0 || s->cc[k].at >= s->cc[best].at)) best = k; }
    if (best >= 0 && now - s->cc[best].at < 15 && (s->cc[best].end < 0 || now < s->cc[best].end)) snprintf(out, n, "%s", s->cc[best].text);
    SDL_UnlockMutex(s->lock);
  }
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
  if (set->autocrop && crop_w == w && crop_h == h && (crop_on.l || crop_on.t)) LINE("CROP|%dx%d (black borders cut)", w - crop_on.l - crop_on.r, h - crop_on.t - crop_on.b);
  else LINE("CROP|%s", set->autocrop ? "none needed" : "off");
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

// The DVB page on now, over the picture: its canvas mapped onto the frame, then as the frame is shown (src: the part
// of the frame on screen, after cropping; dst: where). A bitmap the crop would cut off is moved back inside.
static SDL_Texture *dvb_tex[8]; static SDL_Rect dvb_at[8]; static int dvb_ntex, dvb_page_shown = -1, dvb_cw, dvb_ch;
static void dvb_draw(SDL_Rect src, SDL_Rect dst) {
  double now = last_clock - video_offset;
  SDL_LockMutex(s->lock);
  int best = -1;
  for (int i = 0; i < s->npages; i++) { int k = (s->page_head + i) % DVB_PAGES; if (s->pages[k].at <= now && (best < 0 || s->pages[k].at >= s->pages[best].at)) best = k; }
  int on = best >= 0 && (s->pages[best].end < 0 || now < s->pages[best].end) && now - s->pages[best].at < 60;
  int id = on ? s->pages[best].id : -1;
  if (id != dvb_page_shown) {   // a new page: its bitmaps as textures
    for (int i = 0; i < dvb_ntex; i++) SDL_DestroyTexture(dvb_tex[i]);
    dvb_ntex = 0; dvb_page_shown = id;
    if (on) {
      struct DvbPage *pg = &s->pages[best]; dvb_cw = pg->cw; dvb_ch = pg->ch;
      for (int i = 0; i < pg->n; i++) {
        SDL_Texture *tx = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, pg->r[i].w, pg->r[i].h);
        if (!tx) continue;
        SDL_UpdateTexture(tx, NULL, pg->r[i].px, pg->r[i].w * 4); SDL_SetTextureBlendMode(tx, SDL_BLENDMODE_BLEND);
        dvb_tex[dvb_ntex] = tx; dvb_at[dvb_ntex] = (SDL_Rect){ pg->r[i].x, pg->r[i].y, pg->r[i].w, pg->r[i].h }; dvb_ntex++;
      }
    }
  }
  SDL_UnlockMutex(s->lock);
  if (!dvb_ntex || !src.w || !src.h) return;
  double fx = (double)tex_w / dvb_cw, fy = (double)tex_h / dvb_ch;   // canvas -> frame
  double sx = (double)dst.w / src.w, sy = (double)dst.h / src.h;     // frame -> screen
  for (int i = 0; i < dvb_ntex; i++) {
    SDL_Rect r = dvb_at[i], o;
    o.x = dst.x + (int)lround((r.x * fx - src.x) * sx); o.y = dst.y + (int)lround((r.y * fy - src.y) * sy);
    o.w = (int)lround(r.w * fx * sx); o.h = (int)lround(r.h * fy * sy);
    if (o.y + o.h > dst.y + dst.h) o.y = dst.y + dst.h - o.h;   // (cropped away: back inside)
    if (o.y < dst.y) o.y = dst.y;
    if (o.x < dst.x) o.x = dst.x;
    if (o.x + o.w > dst.x + dst.w) o.x = dst.x + dst.w - o.w;
    SDL_RenderCopy(ren, dvb_tex[i], NULL, &o);
  }
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
        stop_stream(s); s = start_stream(cur_ch, audio); started_at = now_t; playing = rebuf = 0; wall_at = 0; video_offset = 0; subs_off();
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
    ui_no_signal(cur_ch->name, fail_reason);
  } else if (!s->connected || !playing) {
    if (!set->plain_tuning) {   // plain: a black screen; the app draws the channel info and LOADING over it
      draw_tuning(ren, itex, iw, ih, ilogo, snow, SDL_GetTicks() - changed_at);
      if (!s->audio_started) { SDL_PauseAudioDevice(audio, 0); snow_audio(audio); }
    }
    if (retries && !set->plain_tuning) {   // reconnecting: say so over the snow (plain: the app says it)
      char m[48]; snprintf(m, sizeof m, "Signal lost · reconnecting (%d/3)", retries);
      ui_notice(m);
    }
  } else {
    // Clock: the audio being heard (end of queued audio minus what's still queued). When the sound runs dry (the
    // Wi-Fi stalls, another audio track is coming), the clock stops and the picture waits with it, then both go on
    // together once enough sound is queued again. Only when the stream carries on without sound (its video read
    // well past its audio, or there is none) does the clock run on the wall, so the picture never freezes for it.
    SDL_LockMutex(s->lock);
    double clock;
    Uint64 nowc = SDL_GetPerformanceCounter();
    int no_sound = !s->has_audio || s->video_read - s->audio_pts > 2.0;
    if (!rebuf && !no_sound && queued <= 0.02) {
      double base = s->heavy ? AUDIO_START_HEAVY : AUDIO_START;
      rebuf_need = rebuf_last && SDL_GetTicks() - rebuf_last < 10000 ? (rebuf_need * 2 > 4 ? 4 : rebuf_need * 2) : base;
      if (rebuf_need < base) rebuf_need = base;
      rebuf = 1; SDL_PauseAudioDevice(audio, 1);
      fprintf(stderr, "audio ran dry: clock paused at %.2f s, waiting for %.1f s of sound\n", wall_clock, rebuf_need);
    } else if (rebuf && (no_sound || (queued >= rebuf_need && (s->count == VQ || queued >= 2.0)))) {
      rebuf_last = SDL_GetTicks();
      // (the picture ready too: its frames come out of the decoder a little after the sound, and starting
      // without them means dropping the ones that come late)
      rebuf = 0; SDL_PauseAudioDevice(audio, 0);
      fprintf(stderr, "audio back (%.0f ms queued): clock goes on\n", queued * 1000);
    }
    if (rebuf && wall_at) { clock = wall_clock; wall_at = nowc; }   // stopped
    else if (s->has_audio && queued > 0.02) {
      // The queued amount only drops a whole audio chunk (~21 ms) at a time, so the raw audio clock moves in steps,
      // which bunches up 50 fps frames and drops some. Run smoothly on the timer and ease towards the audio clock;
      // snap only when they are far apart (a new channel, another audio track).
      double measured = s->audio_pts - queued;
      double predicted = wall_at ? wall_clock + (double)(nowc - wall_at) / SDL_GetPerformanceFrequency() : measured;
      double diff = measured - predicted;
      clock = diff > 0.08 || diff < -0.08 ? measured : predicted + diff * 0.05;
      wall_clock = clock; wall_at = nowc;
    }
    else {
      if (!wall_at) { wall_clock = s->count ? s->frames[s->head]->pts / 1e6 : 0; wall_at = nowc; }
      clock = wall_clock + (double)(nowc - wall_at) / SDL_GetPerformanceFrequency();
      wall_clock = clock; wall_at = nowc;
    }
    last_clock = clock;
    AVFrame *show = NULL;
    // Timestamp jumps are joined in the demuxer (ts_join), so the picture and the sound share one timeline: a
    // picture behind the sound drops frames until it catches up, one ahead waits. (HLS with the audio in a playlist
    // of its own can start them a segment apart.) Only a frame more than 10 s from the clock, a jump that wasn't
    // joined, is shown now and the picture re-anchored (video_offset): that keeps an offset, so it's the last resort.
    while (s->count) {
      double d = s->frames[s->head]->pts / 1e6 + video_offset - clock;
      if (d > 10 || d < -10) {
        fprintf(stderr, "picture %.2f s from the sound: re-anchored\n", d);
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
    int h = s->h; double sar = s->sar;
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
      crop_update(show); shown_at = SDL_GetTicks();
      av_frame_free(&show); st_shown++; osd_frames++;
    }
    if (video && h) {  // fit to the screen: 16:9 fills the Smart Pro's, letterboxed on the Brick's 4:3 (after cropping black bars, when that's on)
      SDL_Rect src = { 0, 0, tex_w, tex_h };
      if (set->autocrop && crop_w == tex_w && crop_h == tex_h)
        src = (SDL_Rect){ crop_on.l, crop_on.t, tex_w - crop_on.l - crop_on.r, tex_h - crop_on.t - crop_on.b };
      double aspect = (double)src.w * sar / src.h, sw = SCREEN_W, sh = SCREEN_W / aspect;
      if (sh > SCREEN_H) { sh = SCREEN_H; sw = SCREEN_H * aspect; }
      SDL_Rect dst = { (int)((SCREEN_W - sw) / 2), (int)((SCREEN_H - sh) / 2), (int)sw, (int)sh };
      SDL_RenderCopy(ren, video, &src, &dst);
      if (sub_on >= SUB_DVB) dvb_draw(src, dst);
    }
  }
  if (SDL_GetTicks() - osd_at >= 1000) { osd_fps = osd_frames * 1000.0 / (SDL_GetTicks() - osd_at + 1); osd_frames = 0; osd_at = SDL_GetTicks(); }
  if (overlays[set->overlay % NOVERLAYS]) SDL_RenderCopy(ren, overlays[set->overlay % NOVERLAYS], NULL, NULL);

  static Uint32 st_last = 0;
  Uint32 now = SDL_GetTicks();
  if (now - st_last >= 5000 && playing) {
    double sec = (now - st_last) / 1000.0;
    double aq = SDL_GetQueuedAudioSize(audio) / (double)(AUDIO_RATE * 4);
    SDL_LockMutex(s->lock); int q = s->count, w = s->w, h = s->h, vp = s->vp_count; double vahead = s->video_read - (s->audio_pts - aq); SDL_UnlockMutex(s->lock);
    fprintf(stderr, "stats %dx%d: decoded %.1f fps, shown %.1f fps, dropped %d, jumps %d, upload %.1f ms, vqueue %d, audio %.0f ms, vpkts %d, video read %.1f s ahead\n",
            w, h, st_decoded / sec, st_shown / sec, st_dropped, st_jumps, st_shown ? st_upload_ms / st_shown : 0, q,
            aq * 1000, vp, vahead);
    st_decoded = st_shown = st_dropped = st_loops = st_jumps = 0; st_upload_ms = st_present_ms = 0; st_last = now;
  }
}
