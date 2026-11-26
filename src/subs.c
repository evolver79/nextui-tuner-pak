// WebVTT subtitles beside an HLS stream: the master playlist's subtitle renditions, then a thread per chosen track
// that follows its playlist (live: new segments as they come; on demand: what's coming up), parses the cues and
// keeps them, timed on the MPEG-TS clock (X-TIMESTAMP-MAP: MPEGTS is where the segment's LOCAL time sits).
#include "subs.h"
#include <SDL2/SDL.h>
#include <libavformat/avio.h>
#include <libavutil/opt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TS_PERIOD 95443.7176888889   // MPEG-TS timestamps wrap at 2^33 / 90 kHz
#define NCUES 160

// Downloads url into a NUL-terminated buffer; *final (if given) gets where it ended up (after redirects), for
// resolving relative addresses.
static char *get(const char *url, const char *ua, char *final, size_t fn) {
  AVIOContext *io = NULL; AVDictionary *opt = NULL;
  av_dict_set(&opt, "rw_timeout", "10000000", 0);
  if (ua && ua[0]) av_dict_set(&opt, "user_agent", ua, 0);
  char *buf = NULL; size_t n = 0, cap = 0;
  if (final) snprintf(final, fn, "%s", url);
  int ret = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opt);
  if (ret < 0) {   // why, with the address up to its query (no tokens in the log)
    char e[96]; av_strerror(ret, e, sizeof e);
    int l = (int)strcspn(url, "?"); fprintf(stderr, "subtitles: %s (%.*s)\n", e, l > 120 ? 120 : l, url);
  }
  if (ret >= 0) {
    unsigned char chunk[16384]; int r;
    while ((r = avio_read(io, chunk, sizeof chunk)) > 0 && n < 4 << 20) {
      if (n + r + 1 > cap) { cap = (n + r + 1) * 2; buf = realloc(buf, cap); }
      memcpy(buf + n, chunk, r); n += r;
      // only playlists and WebVTT: anything else (a video stream itself) is left at its first bytes
      const char *b = buf; if (n >= 3 && (unsigned char)b[0] == 0xef) b += 3;
      while (b < buf + n && (*b == ' ' || *b == '\r' || *b == '\n')) b++;
      if (buf + n - b >= 7 && strncmp(b, "#EXTM3U", 7) && strncmp(b, "WEBVTT", 6)) { n = 0; break; }
    }
    uint8_t *loc = NULL;
    if (final && av_opt_get(io, "location", AV_OPT_SEARCH_CHILDREN, &loc) >= 0 && loc && loc[0]) snprintf(final, fn, "%s", (char *)loc);
    av_free(loc);
    avio_closep(&io);
  }
  av_dict_free(&opt);
  if (buf && !n) { free(buf); buf = NULL; }
  if (buf) buf[n] = 0;
  return buf;
}

// base + ref -> out (ref absolute, from the host's root, or next to base).
static void resolve(const char *base, const char *ref, char *out, size_t n) {
  if (strstr(ref, "://")) { snprintf(out, n, "%s", ref); return; }
  const char *q = strchr(base, '?'); size_t blen = q ? (size_t)(q - base) : strlen(base);
  if (ref[0] == '/') {
    const char *h = strstr(base, "://"); const char *p = h ? strchr(h + 3, '/') : NULL;
    size_t hl = p ? (size_t)(p - base) : blen;
    snprintf(out, n, "%.*s%s", (int)hl, base, ref);
    return;
  }
  size_t dir = blen; while (dir > 0 && base[dir - 1] != '/') dir--;
  snprintf(out, n, "%.*s%s", (int)dir, base, ref);
}

// ATTR="value" or ATTR=value from an #EXT-X-... line.
static int attr(const char *line, const char *key, char *out, size_t n) {
  size_t kl = strlen(key);
  for (const char *p = line; (p = strstr(p, key)); p += kl) {
    if (p != line && p[-1] != ',' && p[-1] != ':') continue;
    if (p[kl] != '=') continue;
    const char *v = p + kl + 1; size_t l;
    if (*v == '"') { v++; l = strcspn(v, "\""); } else l = strcspn(v, ",\r\n");
    snprintf(out, n, "%.*s", (int)l, v);
    return 1;
  }
  return 0;
}

int vtt_find(const char *url, const char *ua, VttTrack *out, int max) {
  char base[URL_MAX];
  char *m = get(url, ua, base, sizeof base);
  if (!m) return 0;
  int k = 0;
  if (strstr(m, "#EXT-X-STREAM-INF")) {
    char *save = NULL;
    for (char *line = strtok_r(m, "\r\n", &save); line && k < max; line = strtok_r(NULL, "\r\n", &save)) {
      if (strncmp(line, "#EXT-X-MEDIA:", 13)) continue;
      char type[16] = "", uri[URL_MAX] = "", lang[16] = "", name[64] = "";
      attr(line, "TYPE", type, sizeof type);
      if (strcmp(type, "SUBTITLES") || !attr(line, "URI", uri, sizeof uri)) continue;
      attr(line, "LANGUAGE", lang, sizeof lang); attr(line, "NAME", name, sizeof name);
      int dup = 0;   // the same subtitles listed once per group (one group per video variant)
      for (int i = 0; i < k; i++) if (!strcmp(out[i].lang, lang) && !strcmp(out[i].name, name)) dup = 1;
      if (dup) continue;
      VttTrack *t = &out[k++];
      snprintf(t->lang, sizeof t->lang, "%s", lang); snprintf(t->name, sizeof t->name, "%s", name[0] ? name : lang);
      resolve(base, uri, t->uri, sizeof t->uri);
    }
  }
  free(m);
  return k;
}

// --- the thread ---------------------------------------------------------------------------------------------------
typedef struct { double start, end; char text[256]; } Cue;
struct Vtt {
  VttTrack t; char ua[256];
  SDL_Thread *th; SDL_mutex *lock; volatile int stop;
  Cue cues[NCUES]; int ncues;
  double now;   // where playback is (MPEG-TS clock), from vtt_text_at; -1 not yet
};

static double wrapdiff(double a, double b) { return remainder(a - b, TS_PERIOD); }   // a - b, nearest round the wrap

// [hh:]mm:ss.mmm -> seconds; *end after it. -1 if not a time.
static double vtt_time(const char *s, const char **end) {
  int a = 0, b = 0, c = 0, ms = 0, n = 0;
  if (sscanf(s, "%d:%d:%d.%d%n", &a, &b, &c, &ms, &n) == 4) { if (end) *end = s + n; return a * 3600 + b * 60 + c + ms / 1000.0; }
  if (sscanf(s, "%d:%d.%d%n", &b, &c, &ms, &n) == 3) { if (end) *end = s + n; return b * 60 + c + ms / 1000.0; }
  return -1;
}

// Cue text: tags (<i>, <c.yellow>, <v Bob>) out, a few entities decoded, lines kept.
static void clean(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = in; *p && o + 1 < n; ) {
    if (*p == '<') { const char *e = strchr(p, '>'); if (e) { p = e + 1; continue; } }
    if (*p == '&') {
      static const char *ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&nbsp;", " " }, { "&quot;", "\"" }, { "&#39;", "'" } };
      int hit = 0;
      for (int i = 0; i < 6; i++) { size_t l = strlen(ent[i][0]); if (!strncmp(p, ent[i][0], l)) { out[o++] = ent[i][1][0]; p += l; hit = 1; break; } }
      if (hit) continue;
    }
    out[o++] = *p++;
  }
  out[o] = 0;
}

static void add_cue(Vtt *v, double start, double end, const char *text) {
  SDL_LockMutex(v->lock);
  for (int i = 0; i < v->ncues; i++)   // segments overlap (a cue spanning two is in both)
    if (fabs(wrapdiff(v->cues[i].start, start)) < 0.05 && !strcmp(v->cues[i].text, text)) { SDL_UnlockMutex(v->lock); return; }
  int at = v->ncues < NCUES ? v->ncues++ : 0;
  if (at == 0 && v->ncues == NCUES) {   // full: the one that ended longest ago goes
    for (int i = 1; i < NCUES; i++) if (wrapdiff(v->cues[i].end, v->cues[at].end) < 0) at = i;
  }
  v->cues[at].start = start; v->cues[at].end = end; snprintf(v->cues[at].text, sizeof v->cues[at].text, "%s", text);
  SDL_UnlockMutex(v->lock);
}

// One WebVTT segment. Returns where its last cue ends (MPEG-TS clock), or -1.
static double parse_vtt(Vtt *v, char *s) {
  double off = 0, last = -1;   // MPEGTS/90000 - LOCAL (none: the times are the clock's)
  int in_cue = 0; double cs = 0, ce = 0; char text[512] = "";
  // line by line, blank lines kept (they end cues; strtok would skip them)
  for (char *next = s, *line; ; ) {
    line = next;
    if (line) { char *nl = strchr(line, '\n'); if (nl) { *nl = 0; next = nl + 1; } else next = NULL; if (!*line && !next) line = NULL; }
    if (line) { size_t l = strlen(line); if (l && line[l - 1] == '\r') line[l - 1] = 0; }
    int blank = !line || !line[0];
    if (in_cue && blank) {
      char t[256]; clean(text, t, sizeof t);
      if (t[0]) { add_cue(v, cs + off, ce + off, t); last = ce + off; }
      in_cue = 0; text[0] = 0;
    }
    if (!line) break;
    if (!strncmp(line, "X-TIMESTAMP-MAP=", 16)) {
      char *mp = strstr(line, "MPEGTS:"), *lo = strstr(line, "LOCAL:");
      double local = lo ? vtt_time(lo + 6, NULL) : 0;
      if (mp) off = fmod(strtod(mp + 7, NULL) / 90000.0, TS_PERIOD) - (local > 0 ? local : 0);
      continue;
    }
    const char *arrow = strstr(line, "-->");
    if (!in_cue && arrow) {
      cs = vtt_time(line, NULL); ce = vtt_time(arrow + 3 + strspn(arrow + 3, " \t"), NULL);
      if (cs >= 0 && ce >= 0) { in_cue = 1; text[0] = 0; }
      continue;
    }
    if (in_cue && !blank) {
      if (text[0]) strncat(text, "\n", sizeof text - strlen(text) - 1);
      strncat(text, line, sizeof text - strlen(text) - 1);
    }
  }
  return last;
}

static int nap(Vtt *v, int ms) { for (int i = 0; i < ms / 100 && !v->stop; i++) SDL_Delay(100); return !v->stop; }

static int vtt_thread(void *arg) {
  Vtt *v = arg;
  long long next_seq = -1; double ahead = -1;   // the next segment to fetch; where the cues fetched so far reach
  int first = 1, fetched_early = 0;
  while (!v->stop) {
    char base[URL_MAX];
    char *pl = get(v->t.uri, v->ua, base, sizeof base);
    if (!pl) { if (!nap(v, 3000)) break; continue; }
    long long seq = 0; double target = 6; int endlist = strstr(pl, "#EXT-X-ENDLIST") != NULL;
    char *p;
    if ((p = strstr(pl, "#EXT-X-MEDIA-SEQUENCE:"))) seq = atoll(p + 22);
    if ((p = strstr(pl, "#EXT-X-TARGETDURATION:"))) target = atof(p + 22);
    // the segments' addresses, in order
    char *segs[2048]; int nseg = 0; char *save = NULL;
    for (char *line = strtok_r(pl, "\r\n", &save); line && nseg < 2048; line = strtok_r(NULL, "\r\n", &save))
      if (line[0] && line[0] != '#') segs[nseg++] = line;
    // live: start a few segments from the end (where the picture plays); on demand: from the start
    if (first) { next_seq = endlist ? seq : seq + (nseg > 4 ? nseg - 4 : 0); first = 0; }
    if (next_seq < seq) next_seq = seq;   // fell behind the live window
    for (; next_seq < seq + nseg && !v->stop; next_seq++) {
      SDL_LockMutex(v->lock); double now = v->now; SDL_UnlockMutex(v->lock);
      if (endlist && now >= 0 && ahead >= 0 && wrapdiff(ahead, now) > 60) break;   // on demand: far enough ahead
      if (now < 0 && fetched_early >= 2) break;   // not playing yet: two segments, then wait for where it plays
      if (now < 0) fetched_early++;
      char url[URL_MAX]; resolve(base, segs[next_seq - seq], url, sizeof url);
      char *seg = get(url, v->ua, NULL, 0);
      if (seg) { double e = parse_vtt(v, seg); if (e >= 0) ahead = e; free(seg); }
      if (!seg) fprintf(stderr, "subtitles: segment %lld failed\n", next_seq);
    }
    free(pl);
    if (endlist && next_seq >= seq + nseg) { if (!nap(v, 10000)) break; continue; }   // all fetched
    SDL_LockMutex(v->lock); double now2 = v->now; SDL_UnlockMutex(v->lock);
    if (now2 < 0) { if (!nap(v, 1500)) break; continue; }   // waiting for playback: look again soon
    if (!nap(v, (int)(target * 1000 / 2) < 2000 ? 2000 : (int)(target * 1000 / 2))) break;
  }
  SDL_DestroyMutex(v->lock); free(v);
  return 0;
}

Vtt *vtt_start(const VttTrack *t, const char *ua) {
  Vtt *v = calloc(1, sizeof *v);
  v->t = *t; snprintf(v->ua, sizeof v->ua, "%s", ua ? ua : "");
  v->lock = SDL_CreateMutex(); v->now = -1;
  v->th = SDL_CreateThread(vtt_thread, "vtt", v);
  return v;
}

// The thread may be in the middle of a download (up to its timeout): it is told to stop and let go; it frees v itself
// when it ends. The caller forgets v.
void vtt_stop(Vtt *v) {
  if (!v) return;
  v->stop = 1;
  SDL_DetachThread(v->th);
}

void vtt_text_at(Vtt *v, double t, char *out, size_t n) {
  out[0] = 0;
  if (!v) return;
  SDL_LockMutex(v->lock);
  v->now = t;
  // cues far from now go: ended long ago, or far ahead (from before a splice: the stream's clock starts over at each)
  for (int i = 0; i < v->ncues; ) {
    if (wrapdiff(v->cues[i].end, t) < -30 || wrapdiff(v->cues[i].start, t) > 300) v->cues[i] = v->cues[--v->ncues];
    else i++;
  }
  const Cue *on[4]; int k = 0;
  for (int i = 0; i < v->ncues && k < 4; i++)
    if (wrapdiff(t, v->cues[i].start) >= 0 && wrapdiff(t, v->cues[i].end + 0.05) < 0) on[k++] = &v->cues[i];   // (+50 ms: no flicker between back-to-back cues)
  for (int a = 0; a < k; a++) for (int b = a + 1; b < k; b++) if (wrapdiff(on[b]->start, on[a]->start) < 0) { const Cue *x = on[a]; on[a] = on[b]; on[b] = x; }
  for (int i = 0; i < k; i++) {
    if (out[0]) strncat(out, "\n", n - strlen(out) - 1);
    strncat(out, on[i]->text, n - strlen(out) - 1);
  }
  SDL_UnlockMutex(v->lock);
}
