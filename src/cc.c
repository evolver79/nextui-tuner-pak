// Closed captions: CEA-608 byte pairs from the SEI of H.264/H.265 video, decoded by ffmpeg's ccaption. See cc.h.
#include "cc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOLD 32            // packets held to sort them into display order
#define HOLD_US 500000     // (B-frames put them up to this far out of order)

typedef struct { int64_t pts; uint8_t d[93]; int n; } CcPkt;
struct CcState { CcPkt q[HOLD]; int n; AVCodecContext *dec; AVPacket *p; };

CcState *cc_open(void) { return calloc(1, sizeof(CcState)); }
void cc_close(CcState *c) {
  if (!c) return;
  if (c->dec) avcodec_free_context(&c->dec);
  av_packet_free(&c->p);
  free(c);
}

// The SEI payloads of one NAL (header skipped, emulation prevention removed): cc triplets into out.
static int from_sei(const uint8_t *b, int len, uint8_t *out, int max) {
  int i = 0, got = 0;
  while (i + 2 <= len && b[i] != 0x80) {   // (0x80: the rbsp trailing bits)
    int type = 0, size = 0;
    while (i < len && b[i] == 0xff) { type += 255; i++; }
    if (i >= len) break;
    type += b[i++];
    while (i < len && b[i] == 0xff) { size += 255; i++; }
    if (i >= len) break;
    size += b[i++];
    if (i + size > len) break;
    const uint8_t *p = b + i;
    // user_data_registered_itu_t_t35: USA (0xb5), ATSC (0x0031), "GA94", cc_data (3)
    if (type == 4 && size >= 10 && p[0] == 0xb5 && p[1] == 0x00 && p[2] == 0x31 && !memcmp(p + 3, "GA94", 4) && p[7] == 0x03) {
      int count = p[8] & 0x1f;
      for (int k = 0; k < count && 10 + 3 * k + 3 <= size && got + 3 <= max; k++) { memcpy(out + got, p + 10 + 3 * k, 3); got += 3; }
    }
    i += size;
  }
  return got;
}

// Every cc triplet in an Annex B packet.
static int from_packet(const AVPacket *pkt, int hevc, uint8_t *out, int max) {
  const uint8_t *end = pkt->data + pkt->size;
  uint8_t rbsp[4096]; int got = 0;
  for (const uint8_t *p = pkt->data; p + 4 < end; ) {
    if (!(p[0] == 0 && p[1] == 0 && p[2] == 1)) { p++; continue; }
    const uint8_t *nal = p + 3, *q = nal;
    while (q + 3 <= end && !(q[0] == 0 && q[1] == 0 && (q[2] == 1 || q[2] == 0))) q++;
    if (q + 3 > end) q = end;
    int type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f, hdr = hevc ? 2 : 1;
    if ((hevc ? type == 39 : type == 6) && q - nal > hdr) {   // SEI (H.265: prefix SEI)
      int n = 0;
      for (const uint8_t *r = nal + hdr; r < q && n + 2 < (int)sizeof rbsp; r++) {
        if (r + 2 < q && r[0] == 0 && r[1] == 0 && r[2] == 3) { rbsp[n++] = 0; rbsp[n++] = 0; r += 2; continue; }
        rbsp[n++] = *r;
      }
      got += from_sei(rbsp, n, out + got, max - got);
    }
    p = q;
  }
  return got;
}

// ccaption's ASS ("0,0,Default,,0,0,0,,{\an7}text\Nmore") as plain lines.
static void plain(const char *ass, char *out, size_t n) {
  const char *t = ass;
  for (int c = 0; c < 8 && t; c++) { t = strchr(t, ','); if (t) t++; }
  if (!t) t = ass;
  size_t o = 0;
  for (; *t && o + 1 < n; t++) {
    if (*t == '{') { const char *e = strchr(t, '}'); if (e) { t = e; continue; } }
    if (*t == '\\' && (t[1] == 'N' || t[1] == 'n')) { out[o++] = '\n'; t++; continue; }
    if (*t == '\\' && t[1] == 'h') { out[o++] = ' '; t++; continue; }
    out[o++] = *t;
  }
  while (o && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
  out[o] = 0;
}

static void decode(CcState *c, const CcPkt *k, CcEmit emit, void *ud) {
  if (!c->dec) {
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    if (!dc || !(c->dec = avcodec_alloc_context3(dc))) return;
    c->dec->pkt_timebase = (AVRational){1, 1000000};
    if (avcodec_open2(c->dec, dc, NULL) < 0) { avcodec_free_context(&c->dec); return; }
    c->p = av_packet_alloc();
  }
  if (av_new_packet(c->p, k->n) < 0) return;
  memcpy(c->p->data, k->d, k->n); c->p->pts = c->p->dts = k->pts;
  AVSubtitle sub; int got = 0;
  if (avcodec_decode_subtitle2(c->dec, &sub, &got, c->p) >= 0 && got) {
    char text[256] = "";
    for (unsigned r = 0; r < sub.num_rects; r++)
      if (sub.rects[r]->ass) {
        char l[200]; plain(sub.rects[r]->ass, l, sizeof l);
        size_t tl = strlen(text); snprintf(text + tl, sizeof text - tl, "%s%s", text[0] && l[0] ? "\n" : "", l);
      }
    double at = (sub.pts != AV_NOPTS_VALUE ? sub.pts / 1e6 : k->pts / 1e6) + sub.start_display_time / 1000.0;
    double end = sub.end_display_time && sub.end_display_time != UINT32_MAX && sub.end_display_time > sub.start_display_time
                 ? at + (sub.end_display_time - sub.start_display_time) / 1000.0 : -1;
    emit(ud, at, end, text);
    avsubtitle_free(&sub);
  }
  av_packet_unref(c->p);
}

static void pop(CcState *c, CcEmit emit, void *ud) {
  decode(c, &c->q[0], emit, ud);
  memmove(c->q, c->q + 1, (c->n - 1) * sizeof *c->q); c->n--;
}

int cc_packet(CcState *c, const AVPacket *pkt, AVRational tb, int hevc, int dec, CcEmit emit, void *ud) {
  uint8_t d[93]; int n = from_packet(pkt, hevc, d, sizeof d), has = 0;
  for (int i = 0; i + 3 <= n; i += 3)   // valid field-1 pairs with something in them
    if ((d[i] & 4) && (d[i] & 3) == 0 && ((d[i + 1] & 0x7f) || (d[i + 2] & 0x7f))) { has = 1; break; }
  if (!dec) { c->n = 0; if (c->dec) avcodec_flush_buffers(c->dec); return has; }
  if (!n || pkt->pts == AV_NOPTS_VALUE) return has;
  int64_t pts = av_rescale_q(pkt->pts, tb, (AVRational){1, 1000000});
  if (c->n == HOLD) pop(c, emit, ud);
  int at = c->n;
  while (at > 0 && c->q[at - 1].pts > pts) at--;
  memmove(c->q + at + 1, c->q + at, (c->n - at) * sizeof *c->q);
  c->q[at].pts = pts; memcpy(c->q[at].d, d, n); c->q[at].n = n; c->n++;
  while (c->n && c->q[c->n - 1].pts - c->q[0].pts > HOLD_US) pop(c, emit, ud);
  return has;
}
