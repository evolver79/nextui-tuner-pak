// The looks: VCR (an 8x8 pixel-font on-screen display over the live picture, the blue screen when nothing plays) and
// Teletext. Both draw the same Page.
#include "ui.h"
#include "tt.h"
#include "tv.h"
#include "store.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include "pix8.h"

#define W 1024
#define H 768
static SDL_Renderer *ren;

// --- the 8x8 pixel font of the game looks: one white strip of glyphs, coloured per string ------------------------
static SDL_Texture *pix_tex;
static int pfont;   // 1 while a pixel look draws: text() and the width helpers use the pixel font
// Extra glyphs drawn for Tuner: play, reverse, stop, ellipsis (U+25B6, U+25C0, U+25A0, U+2026).
static const unsigned char PIX_EXTRA[4][8] = {
  {0x01, 0x07, 0x1f, 0x7f, 0x1f, 0x07, 0x01, 0x00}, {0x80, 0xe0, 0xf8, 0xfe, 0xf8, 0xe0, 0x80, 0x00},
  {0x00, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x00},
};
static void pix_init(void) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 196 * 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return;
  Uint32 *px = s->pixels; int pitch = s->pitch / 4;
  for (int g = 0; g < 196; g++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
    px[y * pitch + g * 8 + x] = ((g < 192 ? PIX8[g][y] : PIX_EXTRA[g - 192][y]) >> x) & 1 ? 0xFFFFFFFF : 0x00FFFFFF;
  pix_tex = SDL_CreateTextureFromSurface(ren, s);
  SDL_SetTextureBlendMode(pix_tex, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(pix_tex, SDL_ScaleModeNearest);
  SDL_FreeSurface(s);
}
static Uint32 utf8_cp(const char **s) {
  const unsigned char *p = (const unsigned char *)*s; Uint32 c = *p++;
  if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
  else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
  else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
  *s = (const char *)p; return c;
}
static int pix_index(Uint32 c) {
  if (c == 0x2013 || c == 0x2014) c = '-'; else if (c == 0x2018 || c == 0x2019) c = '\'';
  else if (c == 0x201C || c == 0x201D) c = '"'; else if (c == 0x2022) c = 0xB7;
  if (c >= 32 && c < 127) return (int)c - 32;
  if (c >= 0xA0 && c <= 0xFF) return (int)c - 0xA0 + 96;
  switch (c) { case 0x25B6: return 192; case 0x25C0: return 193; case 0x25A0: return 194; case 0x2026: return 195; }
  return -1;
}
static int pix_len(const char *s) { int n = 0; while (*s) { utf8_cp(&s); n++; } return n; }
static void pix_draw(int x, int y, int sc, SDL_Color c, const char *s) {
  if (!pix_tex) return;
  SDL_SetTextureColorMod(pix_tex, c.r, c.g, c.b);
  while (*s) {
    const char *at = s; Uint32 cp = utf8_cp(&s); int g = pix_index(cp);
    if (g >= 0) { SDL_Rect src = { g * 8, 0, 8, 8 }, dst = { x, y, 8 * sc, 8 * sc }; SDL_RenderCopy(ren, pix_tex, &src, &dst); }
    else {   // other scripts: that one character in the teletext font, centred in the cell
      char u[8]; int n = (int)(s - at); if (n > 7) n = 7; memcpy(u, at, n); u[n] = 0;
      tt_draw(x + (8 * sc - tt_text_width(8 * sc, u)) / 2, y, 8 * sc, u, c);
    }
    x += 8 * sc;
  }
}
static int pix_scale(int h) { return h < 16 ? 1 : h / 8; }

void themes_init(SDL_Renderer *r) { ren = r; pix_init(); }

static SDL_Color rgb(int r, int g, int b) { return (SDL_Color){ (Uint8)r, (Uint8)g, (Uint8)b, 255 }; }
static void box(int x, int y, int w, int h, int r, int g, int b, int a) {
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(ren, r, g, b, a);
  SDL_Rect q = { x, y, w, h }; SDL_RenderFillRect(ren, &q);
}
static void frame(int x, int y, int w, int h, int t, SDL_Color c) {
  box(x, y, w, t, c.r, c.g, c.b, 255); box(x, y + h - t, w, t, c.r, c.g, c.b, 255);
  box(x, y, t, h, c.r, c.g, c.b, 255); box(x + w - t, y, t, h, c.r, c.g, c.b, 255);
}
static void text(int x, int y, int h, SDL_Color c, const char *s) {
  if (pfont) { int sc = pix_scale(h); pix_draw(x, y + (h - 8 * sc) / 2, sc, c, s); } else tt_draw(x, y, h, s, c);
}
static int tw(int h, const char *s) { return pfont ? pix_len(s) * 8 * pix_scale(h) : tt_text_width(h, s); }
static void fit(char *s, int chars) {   // shorten UTF-8 to `chars` characters
  int n = 0; for (char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) { if (n == chars) { *p = 0; return; } n++; }
}
// Long names tick along a character at a time where you are looking: the selected row (after a second, to the end,
// a pause, back to the start) and the banner (after a moment, once to the end, where it stays).
static Uint32 sel_t, ban_t;
static int still;   // screenshots: names stay at their start
void themes_still(int on) { still = on; }
static void sel_track(const Page *p) {   // when the selection last moved
  static int cur = -1, kind = -1; static char title[64];
  if (p->cursor != cur || (int)p->kind != kind || strcmp(p->title, title)) { cur = p->cursor; kind = p->kind; snprintf(title, sizeof title, "%s", p->title); sel_t = SDL_GetTicks(); }
}
static void ban_track(const Channel *c) { static const Channel *last; if (c != last) { last = c; ban_t = SDL_GetTicks(); } }
// mode 0: just cut to `chars`; 1: the selected row; 2: the banner.
static void tick(char *s, int chars, int mode) {
  int len = 0; for (const char *q = s; *q; q++) if (((unsigned char)*q & 0xC0) != 0x80) len++;
  int steps = len - chars;
  if (mode && !still && steps > 0 && chars > 0) {
    const Uint32 wait = mode == 1 ? 1000 : 1500, step = 250, hold = 1500;
    Uint32 t = SDL_GetTicks() - (mode == 1 ? sel_t : ban_t);
    if (mode == 1) t %= wait + steps * step + hold;
    int off = t < wait ? 0 : (int)((t - wait) / step); if (off > steps) off = steps;
    char *q = s; for (int n = 0; n < off && *q; ) { q++; if (((unsigned char)*q & 0xC0) != 0x80) n++; }
    memmove(s, q, strlen(q) + 1);
  }
  fit(s, chars);
}
static void clock_str(char *b, size_t n, const char *fmt) { time_t t = time(NULL); strftime(b, n, fmt, localtime(&t)); }

// Writes s in lines of at most `chars` characters, breaking at spaces; returns the y after the last line.
// Splits s into at most `max` lines of at most `chars` bytes, breaking at spaces; returns the number of lines.
static int split_lines(const char *s, int chars, char out[][128], int max) {
  char buf[256]; snprintf(buf, sizeof buf, "%s", s);
  char *p = buf; int n = 0;
  if (chars > 127) chars = 127;
  while (n < max) {
    while (*p == ' ') p++;
    if (!*p) break;
    int len = (int)strlen(p), cut = len;
    if (len > chars) { cut = chars; while (cut > 0 && p[cut] != ' ') cut--; if (cut == 0) cut = chars; }
    snprintf(out[n++], 128, "%.*s", cut, p); p += cut;
  }
  return n;
}
static int wrap(int x, int y, int h, int pitch, int chars, int max_lines, SDL_Color c, const char *s) {
  char lines[8][128]; int n = split_lines(s, chars, lines, max_lines < 8 ? max_lines : 8);
  for (int i = 0; i < n; i++, y += pitch) text(x, y, h, c, lines[i]);
  return y;
}
// A test card in the old style (not any broadcaster's): colour bars, a grey grid, a circle, the time.
void draw_testcard(void) {
  box(0, 0, W, H, 90, 90, 90, 255);
  for (int x = 0; x <= W; x += 64) box(x, 0, 2, H, 230, 230, 230, 255);
  for (int y = 0; y <= H; y += 64) box(0, y, W, 2, 230, 230, 230, 255);
  static const int bars[8][3] = { {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0}, {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0} };
  int cx = W / 2, cy = H / 2, r = 300;
  for (int y = cy - r; y < cy + r; y++) {   // circle filled row by row
    int dx = (int)sqrt((double)r * r - (double)(y - cy) * (y - cy));
    int rel = y - (cy - r);
    if (rel < r * 2 / 5) {   // colour bars in the top of the circle
      int bw = dx * 2 / 8 + 1;
      for (int i = 0; i < 8; i++) box(cx - dx + i * bw, y, bw, 1, bars[i][0], bars[i][1], bars[i][2], 255);
    } else if (rel < r * 3 / 5) box(cx - dx, y, dx * 2, 1, 20, 20, 20, 255);
    else {   // grey steps below
      int sw = dx * 2 / 6 + 1;
      for (int i = 0; i < 6; i++) { int v = i * 51; box(cx - dx + i * sw, y, sw, 1, v, v, v, 255); }
    }
  }
  char t[16]; clock_str(t, sizeof t, "%H:%M:%S");
  box(cx - 150, cy - 26, 300, 52, 0, 0, 0, 255);
  text(cx - tt_text_width(36, "TUNER") / 2, cy - 18, 36, rgb(255, 255, 255), "TUNER");
  box(cx - 110, cy + r - 90, 220, 44, 0, 0, 0, 255);
  text(cx - tt_text_width(30, t) / 2, cy + r - 83, 30, rgb(255, 255, 255), t);
}

// --- PIN entry and the search keyboard, shared: a centred box in the theme's colours -----------------------------
typedef struct { SDL_Color bg, fg, hl, hl_fg; int alpha; SDL_Color key; } EntryStyle;   // key: keys and input field (unset: near black)
static void entry_page(const Page *p, const EntryStyle *st) {
  int bw = 860, bh = p->kind == PAGE_PIN ? 300 : 520, bx = (W - bw) / 2, by = (H - bh) / 2;
  SDL_Color kc = st->key.a ? st->key : rgb(30, 30, 30);
  box(bx, by, bw, bh, st->bg.r, st->bg.g, st->bg.b, st->alpha);
  frame(bx, by, bw, bh, 4, st->fg);
  text(bx + 30, by + 24, 40, st->fg, p->title);
  if (p->help[0]) text(bx + 30, by + 76, 22, st->fg, p->help);
  if (p->kind == PAGE_PIN) {
    int dw = 90, x0 = (W - 4 * dw) / 2;
    for (int i = 0; i < 4; i++) {
      int hl = i == p->cursor, x = x0 + i * dw, y = by + 130;
      box(x, y, dw - 16, 110, hl ? st->hl.r : st->key.a ? kc.r : 0, hl ? st->hl.g : st->key.a ? kc.g : 0, hl ? st->hl.b : st->key.a ? kc.b : 0, 255);
      char d[2] = { p->input[i] ? p->input[i] : '-', 0 };
      int dh = pfont ? 64 : 80;   // the pixel font is as wide as it is tall
      text(x + (dw - 16 - tw(dh, d)) / 2, y + 14 + (80 - dh) / 2, dh, hl ? st->hl_fg : st->fg, d);
    }
  } else {
    char shown[80]; snprintf(shown, sizeof shown, "%s_", p->input);
    if (st->key.a) box(bx + 30, by + 110, bw - 60, 56, kc.r, kc.g, kc.b, 255); else box(bx + 30, by + 110, bw - 60, 56, 0, 0, 0, 255);
    text(bx + 44, by + 120, 36, st->fg, shown);
    int k = 0, row = 0, col = 0, kx0 = bx + 40, ky0 = by + 190;
    for (const char *q = p->keys; *q; ) {
      if (*q == '|') { row++; col = 0; q++; continue; }
      char key[16]; int n = 0;
      if (*q == '<') { q++; while (*q && *q != '>' && n < 14) key[n++] = *q++; if (*q == '>') q++; }
      else key[n++] = *q++;
      key[n] = 0;
      int kw = n > 1 ? 110 : 74, x = kx0 + col, y = ky0 + row * 74, hl = k == p->cursor;
      box(x, y, kw - 8, 64, hl ? st->hl.r : kc.r, hl ? st->hl.g : kc.g, hl ? st->hl.b : kc.b, 255);
      text(x + (kw - 8 - tw(n > 1 ? 22 : 40, key)) / 2, y + (n > 1 ? 20 : 12), n > 1 ? 22 : 40, hl ? st->hl_fg : st->fg, key);
      col += kw; k++;
    }
    if (p->status[0]) text(bx + 30, by + bh - 40, 22, st->fg, p->status);
  }
}

// --- helpers of both looks -----------------------------------------------------------------------------------------
// "A Watch" -> "A" and "Watch"
static void split_hint(const char *h, char *key, size_t kn, char *word, size_t wn) {
  const char *sp = strchr(h, ' ');
  if (!sp) { snprintf(key, kn, "%s", h); word[0] = 0; return; }
  snprintf(key, kn, "%.*s", (int)(sp - h), h); snprintf(word, wn, "%s", sp + 1);
}
static void upper(char *s) {   // ASCII and Latin-1 (æøå, é ...)
  for (unsigned char *q = (unsigned char *)s; *q; q++) {
    if (*q >= 'a' && *q <= 'z') *q -= 32;
    else if (*q == 0xC3 && q[1] >= 0xA0 && q[1] <= 0xBE && q[1] != 0xB7) { q[1] -= 0x20; q++; }
  }
}

// ================================================================================================================
// Teletext: the 40x25 grid in the 8 teletext colours. A page you open is searched for first (the header's page
// counter rolls through the pages going by), then it arrives and paints in row by row. P100 has a block-graphics
// masthead, P300 is a real listings page, and the banner is a newsflash: black boxes over the picture.
// ================================================================================================================
static int tx_reveal = TT_ROWS;   // rows of the page shown so far (the rest are still "arriving")
static void tx_header(int page, int searching, const char *title, int dbl_title) {
  tt_clear();
  char c[32]; clock_str(c, sizeof c, "%a %d %b %H:%M:%S");
  tt_printf(1, 0, TT_WHITE, TT_BLACK, 0, "P%d", page);
  if (searching) tt_printf(6, 0, TT_GREEN, TT_BLACK, 0, "P%d", searching);   // the pages going by
  else tt_print(6, 0, TT_YELLOW, TT_BLACK, 0, "TUNER");
  tt_print(TT_COLS - (int)strlen(c), 0, TT_WHITE, TT_BLACK, 0, c);
  if (title) { tt_fill(1, 0, TT_COLS, TT_BLUE); tt_fill(2, 0, TT_COLS, TT_BLUE); tt_print(2, 1, TT_YELLOW, TT_BLUE, dbl_title, title); }
}
// Rows below the reveal line haven't arrived yet: blank them, then draw.
static void tx_render(void) {
  for (int r = tx_reveal; r < TT_ROWS; r++) { if (r == 0) continue; tt_fill(r, 0, TT_COLS, TT_BLACK); tt_print(0, r, TT_BLACK, TT_BLACK, 0, "                                        "); }
  tt_render();
}
// The page search: half a second of the counter rolling after you open a page, then one row every 14 ms.
static int tx_fetch(const Page *p, int *searching) {
  static int last_page = -1, last_kind = -1; static char last_title[64]; static Uint32 at;
  if (p->page_no != last_page || (int)p->kind != last_kind || strcmp(p->title, last_title)) {
    last_page = p->page_no; last_kind = p->kind; snprintf(last_title, sizeof last_title, "%s", p->title); at = SDL_GetTicks();
  }
  Uint32 t = SDL_GetTicks() - at;
  if (t < 450) { *searching = 100 + (int)(t / 12 * 7 + p->page_no) % 800; tx_reveal = 1; return 1; }
  *searching = 0;
  tx_reveal = 1 + (int)((t - 450) / 14); if (tx_reveal > TT_ROWS) tx_reveal = TT_ROWS;
  return 0;
}
// TUNER in mosaic blocks: 2x2 "pixels" per character cell (U+2580 block elements), each letter 4 x 6 pixels.
static const char *const TX_LOGO[6] = {
  "1111 1001 1001 1111 1110",
  "0110 1001 1101 1000 1001",
  "0110 1001 1101 1110 1001",
  "0110 1001 1011 1000 1110",
  "0110 1001 1011 1000 1010",
  "0110 1111 1001 1111 1001" };
static void tx_masthead(int row0, int col0, int fg, int bg) {
  static const char *Q[16] = { " ", "\xe2\x96\x98", "\xe2\x96\x9d", "\xe2\x96\x80", "\xe2\x96\x96", "\xe2\x96\x8c", "\xe2\x96\x9e", "\xe2\x96\x9b",
                               "\xe2\x96\x97", "\xe2\x96\x9a", "\xe2\x96\x90", "\xe2\x96\x9c", "\xe2\x96\x84", "\xe2\x96\x99", "\xe2\x96\x9f", "\xe2\x96\x88" };
  int w = (int)strlen(TX_LOGO[0]);
  for (int ry = 0; ry < 3; ry++) for (int cx = 0; cx * 2 < w; cx++) {
    int px[4]; for (int k = 0; k < 4; k++) { int x = cx * 2 + (k & 1), y = ry * 2 + (k >> 1); px[k] = x < w && TX_LOGO[y][x] == '1'; }
    tt_print(col0 + cx, row0 + ry, fg, bg, 0, Q[px[0] | px[1] << 1 | px[2] << 2 | px[3] << 3]);
  }
}
static void tx_fastext(const Page *p) {   // the four coloured labels on the bottom row
  static const int cols[4] = { TT_RED, TT_GREEN, TT_YELLOW, TT_CYAN };
  int total = 0, n = 0;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { total += (int)strlen(p->hints[i]); n++; }
  int gap = n > 1 ? (TT_COLS - 2 - total) / (n - 1) : 0; if (gap > 4) gap = 4; if (gap < 1) gap = 1;
  int col = 1;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { col = tt_print(col, 24, cols[i], TT_BLACK, 0, p->hints[i]) + gap; }
}
static void tx_time(char *b, size_t n, time_t t) { strftime(b, n, "%H%M", localtime(&t)); }   // teletext times: 1830
static void tx_page(const Page *p) {
  sel_track(p);
  int searching = 0;
  int arriving = p->kind == PAGE_LOADING ? 1 : tx_fetch(p, &searching);
  if (p->kind == PAGE_LOADING) { searching = 100 + (int)(SDL_GetTicks() / 12 * 7) % 800; tx_reveal = TT_ROWS; }
  char title[64]; snprintf(title, sizeof title, "%s", p->title); upper(title);
  int front = p->page_no == 100 && p->kind == PAGE_MENU;
  tx_header(p->page_no, searching, front ? NULL : title, 1);
  if (arriving && p->kind != PAGE_LOADING) { tx_render(); return; }
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { tx_reveal = TT_ROWS; tt_render(); EntryStyle e = { rgb(0, 0, 0), rgb(255, 255, 0), rgb(0, 0, 255), rgb(255, 255, 255), 255 }; entry_page(p, &e); return; }
  if (p->kind == PAGE_LOADING) {
    tt_print(2, 6, TT_WHITE, TT_BLACK, 0, "Searching for the channel lists");
    char lines[3][128]; int n = split_lines(p->status, TT_COLS - 4, lines, 3);
    for (int i = 0; i < n; i++) tt_print(2, 9 + i, TT_YELLOW, TT_BLACK, 0, lines[i]);
    tt_printf(2, 10 + n, TT_GREEN, TT_BLACK, 0, "%c", "|/-\\"[(SDL_GetTicks() / 150) % 4]);
    tx_render(); return;
  }
  if (front) {   // P100: the masthead on a blue band, the date, then the index
    for (int r = 1; r <= 7; r++) tt_fill(r, 0, TT_COLS, TT_BLUE);
    tx_masthead(2, 4, TT_YELLOW, TT_BLUE);
    tt_print(25, 3, TT_WHITE, TT_BLUE, 0, "TELEVISION");
    tt_print(25, 5, TT_CYAN, TT_BLUE, 0, "FREE AND YOURS");
    tt_fill(8, 0, TT_COLS, TT_RED);
    char d[40]; clock_str(d, sizeof d, "%A %d %B"); upper(d); tt_print(1, 8, TT_WHITE, TT_RED, 0, d);
  }
  int top_row = front ? 11 : 4;
  if (p->help[0]) { char h[64]; snprintf(h, sizeof h, "%s", p->help); fit(h, TT_COLS - 2); tt_print(1, front ? 9 : 3, TT_CYAN, TT_BLACK, 0, h); }
  if (p->kind == PAGE_GUIDE) {   // P300: two rows a channel, now on each; the selected one's next below
    int per = 8, top = p->cursor - p->cursor % per; time_t now = time(NULL);
    char sub[16]; snprintf(sub, sizeof sub, "%d/%d", top / per + 1, (p->nchs + per - 1) / per);
    tt_print(TT_COLS - 1 - (int)strlen(sub), 3, TT_WHITE, TT_BLACK, 0, sub);
    for (int i = top; i < p->nchs && i < top + per; i++) {
      Channel *c = p->chs[i]; int row = 4 + (i - top) * 2, hl = i == p->cursor, bg = hl ? TT_BLUE : TT_BLACK;
      tt_fill(row, 0, TT_COLS, bg); tt_fill(row + 1, 0, TT_COLS, bg);
      char nm[160]; snprintf(nm, sizeof nm, "%s", c->name); tick(nm, TT_COLS - 6, hl);
      tt_printf(1, row, hl ? TT_WHITE : TT_GREEN, bg, 0, "%3d", i + 1);
      tt_print(5, row, hl ? TT_YELLOW : TT_CYAN, bg, 0, nm);
      const Programme *cur, *next; now_next(c, now, &cur, &next);
      char l[200], t1[8];
      if (cur) { tx_time(t1, sizeof t1, cur->start); snprintf(l, sizeof l, "%s %s", t1, cur->title); } else snprintf(l, sizeof l, "No listings");
      tick(l, TT_COLS - 6, hl); tt_print(5, row + 1, TT_WHITE, bg, 0, l);
    }
    if (p->cursor >= 0 && p->cursor < p->nchs) {
      const Programme *cur, *next; now_next(p->chs[p->cursor], now, &cur, &next);
      if (next) { char l[200], t1[8]; tx_time(t1, sizeof t1, next->start); snprintf(l, sizeof l, "%s %s", t1, next->title); tick(l, TT_COLS - 7, 1); tt_print(1, 21, TT_MAGENTA, TT_BLACK, 0, "NEXT"); tt_print(6, 21, TT_YELLOW, TT_BLACK, 0, l); }
    }
    if (p->status[0]) tt_print(TT_COLS - 1 - (int)strlen(p->status), 23, TT_WHITE, TT_BLACK, 0, p->status);
    tx_fastext(p); tx_render(); return;
  }
  int rows = front ? 11 : 18;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int row = top_row + i - p->top, hl = i == p->cursor, bg = hl ? TT_BLUE : TT_BLACK;
    if (row > 22) break;
    tt_fill(row, 0, TT_COLS, bg);
    int col = 1;
    if (p->kind == PAGE_LIST) { tt_printf(0, row, hl ? TT_WHITE : TT_GREEN, bg, 0, "%3d", r->number); col = 4; if (r->fav) tt_print(3, row, TT_YELLOW, bg, 0, "*"); }
    else if (p->kind == PAGE_CHECK) { tt_print(1, row, r->checked ? TT_YELLOW : TT_WHITE, bg, 0, r->checked ? "[X]" : "[ ]"); col = 5; }
    else if (r->number) { tt_printf(1, row, hl ? TT_WHITE : TT_GREEN, bg, 0, "%d", r->number); col = 6; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int dlen = (int)strlen(r->detail), last = TT_COLS - 1;   // nothing in the last column
    int right = dlen <= 8 && p->kind == PAGE_MENU;   // a count or value in a menu: at the right edge
    int name_w = !r->detail[0] ? last - col : right ? last - col - dlen - 1 : (p->kind == PAGE_LIST ? 17 : p->kind == PAGE_CHECK ? 16 : 14);
    tick(nm, name_w, hl);
    tt_print(col, row, hl ? TT_WHITE : (p->kind == PAGE_CHECK && r->checked ? TT_YELLOW : TT_WHITE), bg, 0, nm);
    if (r->detail[0]) {
      char d[200]; snprintf(d, sizeof d, "%s", r->detail);
      if (right) tt_print(last - dlen, row, hl ? TT_YELLOW : TT_CYAN, bg, 0, d);
      else { tick(d, last - col - name_w - 1, hl); tt_print(col + name_w + 1, row, hl ? TT_YELLOW : TT_CYAN, bg, 0, d); }
    }
  }
  if (p->status[0]) tt_print(TT_COLS - 1 - (int)strlen(p->status), 23, TT_WHITE, TT_BLACK, 0, p->status);
  tx_fastext(p);
  tx_render();
}
// The newsflash: each line in its own black box over the picture.
static void tx_box_line(int x, int y, int h, SDL_Color c, const char *s) {
  int w = tt_text_width(h, s);
  box(x - 8, y - 4, w + 16, h + 8, 0, 0, 0, 255);
  text(x, y, h, c, s);
}
static void tx_banner(const Channel *c, int number) {
  ban_track(c);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  int y = H - 40 - (cur ? 40 : 0) - (next ? 40 : 0) - 48;
  char l[200], t1[8], t2[8];
  snprintf(l, sizeof l, "P%03d %s%s", number, is_fav(c->key) ? "* " : "", c->name); tick(l, 30, 2);
  tx_box_line(24, y, 40, TT_PALETTE[TT_YELLOW], l); y += 56;
  if (cur) { tx_time(t1, sizeof t1, cur->start); tx_time(t2, sizeof t2, cur->stop); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); tick(l, 36, 2); tx_box_line(24, y, 30, TT_PALETTE[TT_WHITE], l); y += 40; }
  if (next) { tx_time(t1, sizeof t1, next->start); snprintf(l, sizeof l, "%s %s", t1, next->title); fit(l, 36); tx_box_line(24, y, 30, TT_PALETTE[TT_CYAN], l); }
}
// The volume, when it changes: blocks in green on black, teletext style.
static void tx_volume(int v, int max, int muted) {
  int cw = W / TT_COLS, ch = H / TT_ROWS, cells = 20, x = (W - (cells + 9) * cw) / 2, y = 2 * ch;   // at the top: the banner is below
  box(x - cw / 2, y - ch / 2, (cells + 9) * cw + cw, ch * 2, 0, 0, 0, 255);
  text(x, y, ch, TT_PALETTE[TT_YELLOW], muted ? "MUTE" : "VOLUME");
  int on = muted ? 0 : v * cells / (max > 0 ? max : 1);
  for (int i = 0; i < cells; i++) { SDL_Color c = TT_PALETTE[i < on ? TT_GREEN : TT_BLUE]; box(x + (8 + i) * cw + 2, y + 3, cw - 4, ch - 6, c.r, c.g, c.b, 255); }
}
static void tx_toast(const char *m, int line) { box(W - 24 - tt_text_width(28, m) - 8, 22 + line * 40, tt_text_width(28, m) + 16, 36, 0, 0, 0, 255); text(W - 24 - tt_text_width(28, m), 26 + line * 40, 28, TT_PALETTE[TT_YELLOW], m); }

// ================================================================================================================
// VCR: a video recorder's on-screen display. Blocky white characters over the picture, or the blue screen when
// nothing plays. Moving fast through a list winds the tape (FF / REW with a band of tracking noise), a new channel
// comes in with tracking noise, the banner has a tape counter, and loading is TRACKING with 12:00 blinking.
// ================================================================================================================
static const SDL_Color VCR_W = {255, 255, 255, 255}, VCR_Y = {255, 240, 120, 255}, VCR_BLUE = {16, 24, 176, 255},
  VCR_INK = {0, 0, 0, 255}, VCR_DIM = {170, 190, 255, 255};
static int vcr_live;   // drawing over the picture: characters get a black edge
static void vtext(int x, int y, int h, SDL_Color c, const char *s) {   // OSD characters: a black edge over the picture, a shadow on blue
  int o = pix_scale(h) / 2; if (o < 2) o = 2;
  if (vcr_live) { text(x - o, y, h, VCR_INK, s); text(x + o, y, h, VCR_INK, s); text(x, y - o, h, VCR_INK, s); text(x, y + o, h, VCR_INK, s); }
  text(x + o, y + o, h, VCR_INK, s);
  text(x, y, h, c, s);
}
static void vcr_noise(int y, int h) {   // a band of tracking noise: white and grey dashes
  static Uint32 seed = 7;
  for (int i = 0; i < h * 3; i++) {
    seed = seed * 1664525u + 1013904223u;
    int nx = (int)(seed >> 8) % W, ny = y + (int)((seed >> 4) % (Uint32)(h > 0 ? h : 1)), nw = 6 + (int)((seed >> 20) % 70), v = 150 + (int)((seed >> 26) % 106);
    box(nx, ny, nw, 2 + (int)((seed >> 30) % 2), v, v, v, 210);
  }
  box(0, y + h / 2, W, 2, 255, 255, 255, 90);   // the tear line
}
// Winding: the cursor moving several times in under half a second means a button is held.
static int vcr_wind(const Page *p) {   // 0 still, 1 FF, -1 REW
  static int last = -1, dir; static Uint32 times[4]; static int n;
  Uint32 now = SDL_GetTicks();
  if (p->cursor != last) { if (last >= 0) { dir = p->cursor > last ? 1 : -1; times[n++ % 4] = now; } last = p->cursor; }
  int recent = 0; for (int i = 0; i < 4; i++) if (times[i] && now - times[i] < 450) recent++;
  return recent >= 3 ? dir : 0;
}
static void vcr_mode(int wind, const char *idle) {   // the top-left mode: PLAY / STOP / FF / REW
  const char *m = wind > 0 ? "\xe2\x96\xb6\xe2\x96\xb6 FF" : wind < 0 ? "\xe2\x97\x80\xe2\x97\x80 REW" : idle;
  vtext(40, 28, 32, VCR_W, m);
}
static void vcr_clock(int blink) {   // top right; blink: the unset 12:00 of every VCR
  char t[16]; clock_str(t, sizeof t, "%H:%M");
  if (blink) { if ((SDL_GetTicks() / 500) % 2) vtext(W - 40 - tw(32, "12:00"), 28, 32, VCR_W, "12:00"); return; }
  vtext(W - 40 - tw(32, t), 28, 32, VCR_W, t);
}
static void vcr_counter(char *b, size_t n, long secs) { if (secs < 0) secs = 0; snprintf(b, n, "%ld:%02ld:%02ld", secs / 3600, secs / 60 % 60, secs % 60); }
static void vcr_tape(int x, int y, int w, double frac) {   // tape position: a track with the reel position
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  box(x, y + 6, w, 4, 0, 0, 0, 160); box(x, y + 4, w, 4, 255, 255, 255, 255);
  int fx = x + (int)(w * frac);
  box(x, y + 2, fx - x, 8, VCR_Y.r, VCR_Y.g, VCR_Y.b, 255);
  box(fx - 6, y - 4, 12, 20, 0, 0, 0, 200); box(fx - 4, y - 2, 8, 16, 255, 255, 255, 255);
}
static void vcr_hints(const Page *p, int y) {
  int hx = 40;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) {
    char key[16], word[32]; split_hint(p->hints[i], key, sizeof key, word, sizeof word); upper(word);
    int kw = tw(24, key);
    box(hx, y - 4, kw + 12, 32, 255, 255, 255, 255); text(hx + 6, y, 24, vcr_live ? VCR_INK : VCR_BLUE, key);
    vtext(hx + kw + 22, y, 24, VCR_W, word); hx += kw + 22 + tw(24, word) + 36;
  }
}
static void vcr_page_(const Page *p) {
  sel_track(p);
  vcr_live = p->tv_on;
  if (vcr_live) box(0, 0, W, H, 0, 0, 30, 120); else box(0, 0, W, H, VCR_BLUE.r, VCR_BLUE.g, VCR_BLUE.b, 255);
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) {
    EntryStyle e = { VCR_BLUE, VCR_W, VCR_W, VCR_BLUE, 255, rgb(0, 0, 110) };
    vcr_mode(0, "MENU"); vcr_clock(0); entry_page(p, &e); return;
  }
  int wind = p->kind == PAGE_LIST || p->kind == PAGE_GUIDE ? vcr_wind(p) : 0;
  if (p->kind == PAGE_LOADING) {
    vcr_mode(0, "\xe2\x96\xa0 STOP"); vcr_clock(1);
    vtext(40, 200, 48, VCR_W, "TRACKING");
    static const char *dots[] = { "", ".", "..", "..." }; vtext(40 + tw(48, "TRACKING"), 200, 48, VCR_W, dots[(SDL_GetTicks() / 400) % 4]);
    char up[160]; snprintf(up, sizeof up, "%s", p->status); upper(up);
    wrap(40, 300, 24, 36, (W - 80) / 24, 3, VCR_W, up);
    int band = (int)(SDL_GetTicks() / 6 % (H + 120)) - 60; vcr_noise(band < 0 ? -band : band, 40);
    return;
  }
  vcr_mode(wind, vcr_live ? "\xe2\x96\xb6 PLAY" : "MENU"); vcr_clock(0);
  char ti[64]; snprintf(ti, sizeof ti, "%s", p->title); upper(ti); fit(ti, 24);
  vtext(40, 84, 40, VCR_W, ti);
  int y = 140;
  if (p->help[0]) { char h[160]; snprintf(h, sizeof h, "%s", p->help); upper(h); fit(h, 38); vtext(40, y, 24, VCR_DIM, h); }
  y += 40;
  const Row *sel = p->cursor >= 0 && p->cursor < p->nrows ? &p->rows[p->cursor] : NULL;
  if (p->kind == PAGE_GUIDE) {   // now and next per channel, two lines each
    int rows = 8, top = p->cursor - p->cursor % rows, rh = 60; time_t now = time(NULL);
    for (int i = top; i < p->nchs && i < top + rows; i++) {
      Channel *c = p->chs[i]; int ry = y + (i - top) * rh, hl = i == p->cursor;
      if (hl) box(28, ry - 6, W - 56, rh - 4, 255, 255, 255, 255);
      else if ((i - top) % 2) box(28, ry - 6, W - 56, rh - 4, 255, 255, 255, vcr_live ? 30 : 22);
      SDL_Color fg = hl ? VCR_BLUE : VCR_W, fy = hl ? VCR_INK : VCR_Y;
      char nm[160]; snprintf(nm, sizeof nm, "%03d %s", i + 1, c->name); upper(nm); tick(nm, 37, hl);
      if (hl) text(40, ry, 24, fg, nm); else vtext(40, ry, 24, fg, nm);
      const Programme *cur, *next; now_next(c, now, &cur, &next);
      char l[200], t1[8], t2[8];
      if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); }
      else snprintf(l, sizeof l, "NO GUIDE");
      upper(l);
      tick(l, 37, hl); if (hl) text(40, ry + 26, 24, fy, l); else vtext(40, ry + 26, 24, fy, l);
    }
    if (wind) vcr_noise(y + 180, 44);
    if (p->cursor >= 0 && p->cursor < p->nchs) {   // the selected channel's next programme
      const Programme *cur, *next; now_next(p->chs[p->cursor], now, &cur, &next);
      if (next) { char l[200], t1[8]; strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "NEXT %s %s", t1, next->title); upper(l); tick(l, 38, 1); vtext(40, 676, 24, VCR_DIM, l); }
    }
    vcr_hints(p, 724);
    return;
  }
  int rows = 10, rh = 44;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * rh, hl = i == p->cursor;
    if (hl) box(28, ry - 6, W - 56, rh, 255, 255, 255, 255);
    SDL_Color fg = hl ? VCR_BLUE : VCR_W;
    #define VT(x, yy, h, c, s) do { if (hl) text(x, yy, h, c, s); else vtext(x, yy, h, c, s); } while (0)
    int x = 40;
    if (hl) VT(x, ry, 32, fg, "\xe2\x96\xb6");
    x += 40;
    if (p->kind == PAGE_LIST) { char n[8]; snprintf(n, sizeof n, "%03d", r->number); VT(x, ry, 32, hl ? VCR_BLUE : VCR_DIM, n); x += tw(32, "000 "); }
    if (p->kind == PAGE_CHECK) { VT(x, ry, 32, hl ? VCR_BLUE : r->checked ? VCR_Y : VCR_DIM, r->checked ? "ON " : "OFF"); x += tw(32, "OFF "); }
    if (p->kind == PAGE_LIST) { if (r->fav) VT(x, ry, 32, hl ? VCR_BLUE : VCR_Y, "*"); x += tw(32, "*"); }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text); upper(nm);
    int right = W - 48;
    int shortd = r->detail[0] && p->kind != PAGE_LIST && strlen(r->detail) <= 8;
    if (shortd) { char d[16]; snprintf(d, sizeof d, "%s", r->detail); upper(d); int dw = tw(32, d); tick(nm, (right - dw - 32 - x) / 32, hl); VT(x, ry, 32, fg, nm); VT(right - dw, ry, 32, hl ? VCR_BLUE : VCR_Y, d); }
    else { tick(nm, (right - x) / 32, hl); VT(x, ry, 32, fg, nm); }
    #undef VT
  }
  if (wind) vcr_noise(y + 160 + (int)(SDL_GetTicks() / 3 % 120), 48);
  // the info strip: the selected row's programme (or longer value) and the position
  int iy = 628;
  box(28, iy - 8, W - 56, 3, 255, 255, 255, vcr_live ? 200 : 255);
  if (sel && sel->detail[0] && !(p->kind != PAGE_LIST && strlen(sel->detail) <= 8)) {
    char d[200]; snprintf(d, sizeof d, "%s%s", p->kind == PAGE_LIST ? "\xe2\x96\xb6 " : "", sel->detail); upper(d);
    int sw = p->status[0] && p->kind == PAGE_LIST ? tw(24, p->status) + 24 : 0;
    tick(d, (W - 80 - sw) / 24, 1); vtext(40, iy + 8, 24, VCR_Y, d);
  }
  if (p->status[0] && p->kind == PAGE_LIST) vtext(W - 40 - tw(24, p->status), iy + 8, 24, VCR_W, p->status);
  vcr_hints(p, 724);
}
// The volume, when it changes: a VCR's on-screen bar of blocks.
static void vcr_volume(int v, int max, int muted) {
  pfont = 1; vcr_live = 1;
  int cells = 20, cw = 32, x = (W - cells * cw) / 2, y = H / 2 - 10;   // mid-screen, clear of the banner
  vtext(x, y - 48, 32, VCR_W, muted ? "MUTE" : "VOLUME");
  char n[8]; snprintf(n, sizeof n, "%d", v); if (!muted) vtext(x + cells * cw - tw(32, n), y - 48, 32, VCR_W, n);
  int on = muted ? 0 : v * cells / (max > 0 ? max : 1);
  for (int i = 0; i < cells; i++) {
    box(x + i * cw - 2, y - 2, cw - 4, 44, 0, 0, 0, 200);
    if (i < on) box(x + i * cw, y, cw - 8, 40, 255, 255, 255, 255); else box(x + i * cw, y + 30, cw - 8, 10, 255, 255, 255, 255);
  }
  pfont = 0;
}
static void vcr_page(const Page *p) { pfont = 1; vcr_page_(p); pfont = 0; }
static void vcr_banner(const Channel *c, int number) {
  pfont = 1; ban_track(c); vcr_live = 1;
  Uint32 since = SDL_GetTicks() - ban_t;
  if (since < 900) {   // the new channel comes in with tracking noise that rolls away
    int y = (int)(since * H / 900); vcr_noise(y, 70); if (since < 400) vcr_noise(H - y, 40);
  }
  vtext(40, 28, 32, VCR_W, "\xe2\x96\xb6 PLAY");
  char n[16]; snprintf(n, sizeof n, number < 100 ? "CH %02d" : "CH%d", number);
  vtext(W - 40 - tw(64, n), 24, 64, VCR_W, n);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  box(0, H - 210, W, 210, 0, 0, 0, 110);
  char nm[160]; snprintf(nm, sizeof nm, "%s%s", is_fav(c->key) ? "* " : "", c->name); upper(nm); tick(nm, (W - 80) / 40, 2);
  vtext(40, H - 196, 40, VCR_W, nm);
  if (cur) {
    char l[200]; snprintf(l, sizeof l, "%s", cur->title); upper(l); tick(l, (W - 80) / 24, 2); vtext(40, H - 140, 24, VCR_Y, l);
    time_t now = time(NULL); long len = (long)(cur->stop - cur->start); if (len < 1) len = 1;
    char ctr[24]; vcr_counter(ctr, sizeof ctr, (long)(now - cur->start));
    vtext(40, H - 96, 32, VCR_W, ctr);
    int tx = 40 + tw(32, "0:00:00") + 32; vcr_tape(tx, H - 88, W - 40 - tx, (double)(now - cur->start) / len);
  }
  if (next) { char l[200], t1[8]; strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "NEXT %s %s", t1, next->title); upper(l); tick(l, (W - 80) / 24, 2); vtext(40, H - 48, 24, VCR_DIM, l); }
  pfont = 0;
}
static void vcr_toast(const char *m, int line) {
  pfont = 1; vcr_live = 1; char b[96]; snprintf(b, sizeof b, "%s", m); upper(b);
  vtext(W - 40 - tw(32, b), 112 + line * 48, 32, VCR_W, b);
  pfont = 0;
}

static const Theme T_TEXT = { "Teletext", 18, 0, tx_page, tx_banner, tx_toast, tx_volume };
static const Theme T_VCR = { "VCR", 10, 1, vcr_page, vcr_banner, vcr_toast, vcr_volume };
const Theme *THEMES[] = { &T_VCR, &T_TEXT };
const int NTHEMES = 2;
