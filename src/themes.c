// The looks: CRT TV (menus over the live picture), Teletext, VCR, Cable guide, and three game looks (8-bit Quest,
// 16-bit RPG, Pocket) in an 8x8 pixel font. They all draw the same Page.
#include "ui.h"
#include "tt.h"
#include "tv.h"
#include "store.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <dirent.h>
#include "pix8.h"

#define W 1024
#define H 768
static SDL_Renderer *ren;

// --- the 8x8 pixel font of the game looks: one white strip of glyphs, coloured per string ------------------------
static SDL_Texture *pix_tex;
static int pfont;   // 1 while a game look draws: text() and the width helpers use the pixel font
static void pix_init(void) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 192 * 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return;
  Uint32 *px = s->pixels; int pitch = s->pitch / 4;
  for (int g = 0; g < 192; g++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
    px[y * pitch + g * 8 + x] = (PIX8[g][y] >> x) & 1 ? 0xFFFFFFFF : 0x00FFFFFF;
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
static void textf(int x, int y, int h, SDL_Color c, const char *fmt, ...) {
  char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); text(x, y, h, c, b);
}
static int tw(int h, const char *s) { return pfont ? pix_len(s) * 8 * pix_scale(h) : tt_text_width(h, s); }
static void fit(char *s, int chars) {   // shorten UTF-8 to `chars` characters
  int n = 0; for (char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) { if (n == chars) { *p = 0; return; } n++; }
}
// Long names tick along a character at a time where you are looking: the selected row (after a second, to the end,
// a pause, back to the start) and the banner (after a moment, once to the end, where it stays).
static Uint32 sel_t, ban_t;
static void sel_track(const Page *p) {   // when the selection last moved
  static int cur = -1, kind = -1; static char title[64];
  if (p->cursor != cur || (int)p->kind != kind || strcmp(p->title, title)) { cur = p->cursor; kind = p->kind; snprintf(title, sizeof title, "%s", p->title); sel_t = SDL_GetTicks(); }
}
static void ban_track(const Channel *c) { static const Channel *last; if (c != last) { last = c; ban_t = SDL_GetTicks(); } }
// mode 0: just cut to `chars`; 1: the selected row; 2: the banner.
static void tick(char *s, int chars, int mode) {
  int len = 0; for (const char *q = s; *q; q++) if (((unsigned char)*q & 0xC0) != 0x80) len++;
  int steps = len - chars;
  if (mode && steps > 0 && chars > 0) {
    const Uint32 wait = mode == 1 ? 1000 : 1500, step = 250, hold = 1500;
    Uint32 t = SDL_GetTicks() - (mode == 1 ? sel_t : ban_t);
    if (mode == 1) t %= wait + steps * step + hold;
    int off = t < wait ? 0 : (int)((t - wait) / step); if (off > steps) off = steps;
    char *q = s; for (int n = 0; n < off && *q; ) { q++; if (((unsigned char)*q & 0xC0) != 0x80) n++; }
    memmove(s, q, strlen(q) + 1);
  }
  fit(s, chars);
}
static int chars_for(int width, int h) { return pfont ? width / (8 * pix_scale(h)) : width / (h * 4 / 5); }
static void clock_str(char *b, size_t n, const char *fmt) { time_t t = time(NULL); strftime(b, n, fmt, localtime(&t)); }

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

// --- the guide grid, shared: channels down, half hours across --------------------------------------------------
typedef struct { SDL_Color bg, head, cell, cell_now, text, accent, hl; } GridStyle;
static void guide_grid(const Page *p, int x0, int y0, int w, int h, const GridStyle *st) {
  int name_w = 300, rows = 8, row_h = (h - 40) / rows, span = 2 * 3600;
  time_t now = time(NULL), start = now - now % 1800;
  box(x0, y0, w, h, st->bg.r, st->bg.g, st->bg.b, 235);
  box(x0, y0, w, 36, st->head.r, st->head.g, st->head.b, 255);
  for (int k = 0; k < 4; k++) {
    time_t t = start + k * 1800; char b[8]; strftime(b, sizeof b, "%H:%M", localtime(&t));
    text(x0 + name_w + k * (w - name_w) / 4 + 6, y0 + 6, 24, st->text, b);
  }
  int top = p->cursor - p->cursor % rows;
  for (int i = top; i < p->nchs && i < top + rows; i++) {
    Channel *c = p->chs[i]; int y = y0 + 40 + (i - top) * row_h;
    int hl = i == p->cursor;
    box(x0, y, name_w - 4, row_h - 4, hl ? st->hl.r : st->head.r, hl ? st->hl.g : st->head.g, hl ? st->hl.b : st->head.b, 255);
    char nm[64]; snprintf(nm, sizeof nm, "%d %s", i + 1, c->name); fit(nm, chars_for(name_w - 12, 20));
    text(x0 + 6, y + (row_h - 20) / 2, 20, st->text, nm);
    int any = 0;
    for (int k = 0; k < c->nprogs; k++) {
      Programme *pr = &c->progs[k];
      if (pr->stop <= start || pr->start >= start + span) continue;
      int a = pr->start < start ? 0 : (int)((pr->start - start) * (w - name_w) / span);
      int b = pr->stop > start + span ? w - name_w : (int)((pr->stop - start) * (w - name_w) / span);
      if (b - a < 6) continue;
      int on = pr->start <= now && now < pr->stop;
      SDL_Color cc = on ? st->cell_now : st->cell;
      box(x0 + name_w + a, y, b - a - 4, row_h - 4, cc.r, cc.g, cc.b, 255);
      char t[128]; snprintf(t, sizeof t, "%s", pr->title); fit(t, chars_for(b - a - 14, 20));
      text(x0 + name_w + a + 6, y + (row_h - 20) / 2, 20, st->text, t);
      any = 1;
    }
    if (!any) { box(x0 + name_w, y, w - name_w - 4, row_h - 4, st->cell.r, st->cell.g, st->cell.b, 160); text(x0 + name_w + 8, y + (row_h - 20) / 2, 20, st->text, "no guide"); }
  }
  // the "now" line
  int nx = x0 + name_w + (int)((now - start) * (w - name_w) / span);
  box(nx, y0 + 36, 3, h - 36, st->accent.r, st->accent.g, st->accent.b, 255);
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

// ================================================================================================================
// CRT TV: a 90s TV set's on-screen menus, over the live picture (or a test card when nothing is tuned)
// ================================================================================================================
static const SDL_Color CRT_GREEN = {90, 255, 90, 255}, CRT_WHITE = {255, 255, 255, 255}, CRT_DIM = {150, 200, 150, 255}, CRT_STAR = {255, 230, 80, 255};
static void crt_page(const Page *p) {
  sel_track(p);
  if (!p->tv_on) { draw_testcard(); box(0, 0, W, H, 0, 0, 0, 110); }
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { EntryStyle e = { rgb(0, 0, 0), CRT_GREEN, rgb(40, 160, 40), CRT_WHITE, 225 }; entry_page(p, &e); return; }
  if (p->kind == PAGE_GUIDE) {
    GridStyle g = { rgb(0, 0, 0), rgb(0, 60, 0), rgb(20, 70, 20), rgb(40, 140, 40), CRT_WHITE, rgb(255, 80, 80), rgb(40, 160, 40) };
    guide_grid(p, 24, 70, W - 48, H - 130, &g);
    text(24, 18, 40, CRT_GREEN, p->title);
    return;
  }
  int bx = 64, by = 60, bw = W - 128, bh = H - 120;
  box(bx, by, bw, bh, 0, 0, 0, 215);
  frame(bx, by, bw, bh, 4, CRT_GREEN);
  text(bx + 28, by + 22, 44, CRT_GREEN, p->title);
  char t[16]; clock_str(t, sizeof t, "%H:%M");
  text(bx + bw - 28 - tt_text_width(32, t), by + 28, 32, CRT_WHITE, t);
  int y = by + 84;
  if (p->help[0]) { text(bx + 28, y, 22, CRT_DIM, p->help); y += 32; }
  if (p->kind == PAGE_LOADING) {
    text(bx + 28, y + 40, 32, CRT_WHITE, p->status);
    static const char *dots[] = { "", ".", "..", "..." };
    text(bx + 28, y + 90, 32, CRT_GREEN, dots[(SDL_GetTicks() / 400) % 4]);
    return;
  }
  int rows = 10, row_h = 50;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * row_h, hl = i == p->cursor;
    if (hl) box(bx + 16, ry - 4, bw - 32, row_h - 4, 40, 160, 40, 230);
    SDL_Color fg = hl ? CRT_WHITE : CRT_GREEN;
    int x = bx + 28;
    if (p->kind == PAGE_LIST) { textf(x, ry + 4, 34, fg, "PR %02d", r->number % 100 == r->number ? r->number : r->number); x += tt_text_width(34, "PR 000") + 10; }
    if (p->kind == PAGE_CHECK) { text(x, ry + 4, 34, fg, r->checked ? "\xe2\x96\xa0" : "\xe2\x96\xa1"); x += 44; }
    if (p->kind == PAGE_LIST) {   // logo box
      if (r->logo) { int lw, lh; SDL_QueryTexture(r->logo, NULL, NULL, &lw, &lh); double k = lw * 34 > lh * 56 ? 56.0 / lw : 34.0 / lh; SDL_Rect d = { x + (56 - (int)(lw * k)) / 2, ry + 4 + (34 - (int)(lh * k)) / 2, (int)(lw * k), (int)(lh * k) }; SDL_RenderCopy(ren, r->logo, NULL, &d); }
      x += 66;
    }
    if (p->kind == PAGE_LIST) { if (r->fav) text(x, ry + 4, 34, hl ? CRT_WHITE : CRT_STAR, "*"); x += tt_text_width(34, "*") + 8; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int avail = bx + bw - 28 - x;
    if (r->detail[0] && p->kind != PAGE_CHECK && strlen(r->detail) <= 8) {   // a count or a short value: right edge
      int dw = tt_text_width(28, r->detail);
      tick(nm, chars_for(avail - dw - 20, 34), hl); text(x, ry + 4, 34, fg, nm);
      text(bx + bw - 28 - dw, ry + 8, 28, hl ? CRT_WHITE : CRT_DIM, r->detail);
    } else if (r->detail[0] && p->kind != PAGE_CHECK) {
      tick(nm, chars_for(avail / 2, 34), hl); text(x, ry + 4, 34, fg, nm);
      char d[200]; snprintf(d, sizeof d, "%s", r->detail); tick(d, chars_for(avail / 2 - 10, 24), hl);
      text(x + avail / 2 + 10, ry + 10, 24, hl ? CRT_WHITE : CRT_DIM, d);
    } else { tick(nm, chars_for(avail, 34), hl); text(x, ry + 4, 34, fg, nm); }
  }
  if (p->nrows > rows) textf(bx + bw - 120, by + bh - 40, 22, CRT_DIM, "%d/%d", p->cursor + 1, p->nrows);
  int hx = bx + 28;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { text(hx, by + bh - 40, 22, CRT_WHITE, p->hints[i]); hx += tt_text_width(22, p->hints[i]) + 30; }
}
static void crt_banner(const Channel *c, int number) {
  ban_track(c);
  char n[16]; snprintf(n, sizeof n, "%02d", number);
  text(W - 40 - tt_text_width(96, n), 30, 96, CRT_GREEN, n);   // the big green channel number of old TVs
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  box(0, H - 150, W, 150, 0, 0, 0, 160);
  char nm[160]; snprintf(nm, sizeof nm, "%s%s", is_fav(c->key) ? "* " : "", c->name); tick(nm, 30, 2);
  text(40, H - 134, 40, CRT_WHITE, nm);
  char t1[8], t2[8], l[200];
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); tick(l, 46, 2); text(40, H - 84, 26, CRT_GREEN, l); }
  if (next) { strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "%s %s", t1, next->title); fit(l, 46); text(40, H - 48, 26, CRT_DIM, l); }
}
static void crt_toast(const char *m, int line) { text(40, 30 + line * 44, 34, CRT_GREEN, m); }

// ================================================================================================================
// Teletext: the 40x25 grid, page numbers, coloured fastext labels
// ================================================================================================================
static void tx_header(int page, const char *title) {
  tt_clear();
  char c[32]; clock_str(c, sizeof c, "%a %d %b %H:%M:%S");
  tt_printf(0, 0, TT_WHITE, TT_BLACK, 0, " P%d", page);
  tt_print(8, 0, TT_YELLOW, TT_BLACK, 0, "TUNER");
  tt_print(TT_COLS - (int)strlen(c), 0, TT_WHITE, TT_BLACK, 0, c);
  tt_fill(1, 0, TT_COLS, TT_BLUE); tt_fill(2, 0, TT_COLS, TT_BLUE);
  tt_print(2, 1, TT_YELLOW, TT_BLUE, 1, title);
}
static void tx_page(const Page *p) {
  sel_track(p);
  char title[64]; snprintf(title, sizeof title, "%s", p->title);
  for (char *q = title; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
  tx_header(p->page_no, title);
  if (p->help[0]) tt_print(1, 3, TT_CYAN, TT_BLACK, 0, p->help);
  if (p->kind == PAGE_GUIDE) {
    tt_render();
    GridStyle g = { rgb(0, 0, 0), rgb(0, 0, 255), rgb(0, 0, 160), rgb(255, 255, 0), rgb(255, 255, 255), rgb(255, 0, 0), rgb(0, 160, 255) };
    // yellow "now" cells need dark text: the grid uses white, which reads fine on the teletext blue/yellow mix
    guide_grid(p, 0, 110, W, H - 150, &g);
    return;
  }
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { tt_render(); EntryStyle e = { rgb(0, 0, 0), rgb(255, 255, 0), rgb(0, 0, 255), rgb(255, 255, 255), 255 }; entry_page(p, &e); return; }
  if (p->kind == PAGE_LOADING) {
    tt_print(2, 8, TT_WHITE, TT_BLACK, 0, "Fetching the channel lists.");
    tt_print(2, 10, TT_YELLOW, TT_BLACK, 0, p->status);
    tt_printf(2, 12, TT_GREEN, TT_BLACK, 0, "%c", "|/-\\"[(SDL_GetTicks() / 150) % 4]);
    tt_render(); return;
  }
  int top_row = 4;
  for (int i = p->top; i < p->nrows && i < p->top + 18; i++) {
    const Row *r = &p->rows[i]; int row = top_row + i - p->top, hl = i == p->cursor, bg = hl ? TT_BLUE : TT_BLACK;
    tt_fill(row, 0, TT_COLS, bg);
    int col = 1;
    if (p->kind == PAGE_LIST) { tt_printf(0, row, hl ? TT_WHITE : TT_GREEN, bg, 0, "%3d", r->number); col = 4; if (r->fav) tt_print(3, row, TT_YELLOW, bg, 0, "*"); }
    else if (p->kind == PAGE_CHECK) { tt_print(1, row, r->checked ? TT_YELLOW : TT_WHITE, bg, 0, r->checked ? "[X]" : "[ ]"); col = 5; }
    else if (r->number) { tt_printf(1, row, hl ? TT_WHITE : TT_GREEN, bg, 0, "%d", r->number); col = 6; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int dlen = (int)strlen(r->detail);
    int name_w = !r->detail[0] ? TT_COLS - col - 1 : dlen <= 8 && p->kind != PAGE_LIST ? TT_COLS - col - dlen - 2 : (p->kind == PAGE_LIST ? 17 : 14);
    tick(nm, name_w, hl);
    tt_print(col, row, hl ? TT_WHITE : (p->kind == PAGE_CHECK && r->checked ? TT_YELLOW : TT_WHITE), bg, 0, nm);
    if (r->detail[0]) { char d[200]; snprintf(d, sizeof d, "%s", r->detail); tick(d, TT_COLS - col - name_w - 1, hl); tt_print(col + name_w + 1, row, hl ? TT_YELLOW : TT_CYAN, bg, 0, d); }
  }
  if (p->status[0]) tt_print(TT_COLS - (int)strlen(p->status), 23, TT_WHITE, TT_BLACK, 0, p->status);
  int cols[4] = { TT_RED, TT_GREEN, TT_YELLOW, TT_CYAN };
  for (int i = 0; i < 4; i++) if (p->hints[i]) tt_print(i * 10, 24, cols[i], TT_BLACK, 0, p->hints[i]);
  tt_render();
}
static void tx_banner(const Channel *c, int number) {
  ban_track(c);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  int h = 34 + (cur ? 34 : 0) + (next ? 34 : 0) + 16;
  box(0, H - h, W, h, 0, 0, 0, 255);
  char l[200]; snprintf(l, sizeof l, "P%03d %s%s", number, is_fav(c->key) ? "* " : "", c->name); tick(l, 38, 2);
  text(16, H - h + 8, 30, TT_PALETTE[TT_YELLOW], l);
  char t1[8], t2[8]; int y = H - h + 44;
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); tick(l, 40, 2); text(16, y, 28, TT_PALETTE[TT_WHITE], l); y += 34; }
  if (next) { strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "%s %s", t1, next->title); fit(l, 40); text(16, y, 28, TT_PALETTE[TT_CYAN], l); }
}
static void tx_toast(const char *m, int line) { box(W - 24 - tt_text_width(28, m) - 8, 22 + line * 40, tt_text_width(28, m) + 16, 36, 0, 0, 0, 255); text(W - 24 - tt_text_width(28, m), 26 + line * 40, 28, TT_PALETTE[TT_YELLOW], m); }

// ================================================================================================================
// VCR: the video recorder's blue screen with white blocky text
// ================================================================================================================
static const SDL_Color VCR_W = {255, 255, 255, 255}, VCR_Y = {255, 255, 120, 255};
static void vcr_page(const Page *p) {
  sel_track(p);
  box(0, 0, W, H, 0, 0, 170, 255);
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { EntryStyle e = { rgb(0, 0, 170), VCR_W, rgb(255, 255, 255), rgb(0, 0, 170), 255 }; entry_page(p, &e); return; }
  char t[24]; clock_str(t, sizeof t, "%H:%M");
  text(40, 30, 40, VCR_W, p->title);
  text(W - 40 - tt_text_width(40, t), 30, 40, VCR_W, t);
  box(40, 84, W - 80, 4, 255, 255, 255, 255);
  int y = 110;
  if (p->help[0]) { text(40, y, 24, VCR_Y, p->help); y += 40; }
  if (p->kind == PAGE_GUIDE) { GridStyle g = { rgb(0, 0, 140), rgb(0, 0, 90), rgb(0, 0, 200), rgb(0, 120, 255), VCR_W, rgb(255, 255, 255), rgb(0, 160, 255) }; guide_grid(p, 30, 100, W - 60, H - 150, &g); return; }
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { EntryStyle e = { rgb(0, 0, 170), VCR_W, rgb(255, 255, 255), rgb(0, 0, 170), 255 }; entry_page(p, &e); return; }
  if (p->kind == PAGE_LOADING) { text(40, y + 40, 36, VCR_W, p->status); if ((SDL_GetTicks() / 500) % 2) text(40, y + 100, 36, VCR_W, "\xe2\x96\xb6 LOADING"); return; }
  for (int i = p->top; i < p->nrows && i < p->top + 12; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * 46, hl = i == p->cursor;
    if (hl) text(40, ry, 34, VCR_W, "\xe2\x96\xb6");
    int x = 90;
    if (p->kind == PAGE_LIST) { textf(x, ry, 34, VCR_W, "CH%03d", r->number); x += tt_text_width(34, "CH000 "); }
    if (p->kind == PAGE_CHECK) { text(x, ry, 34, VCR_W, r->checked ? "ON " : "OFF"); x += tt_text_width(34, "OFF "); }
    if (p->kind == PAGE_LIST) { if (r->fav) text(x, ry, 34, VCR_Y, "*"); x += tt_text_width(34, "*") + 8; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int avail = W - 40 - x;
    if (r->detail[0] && strlen(r->detail) <= 8) { int dw = tt_text_width(28, r->detail); tick(nm, chars_for(avail - dw - 20, 34), hl); text(x, ry, 34, VCR_W, nm); text(W - 40 - dw, ry + 4, 28, VCR_Y, r->detail); }
    else if (r->detail[0]) { tick(nm, chars_for(avail / 2, 34), hl); text(x, ry, 34, VCR_W, nm); char d[200]; snprintf(d, sizeof d, "%s", r->detail); tick(d, chars_for(avail / 2 - 10, 24), hl); text(x + avail / 2 + 10, ry + 6, 24, VCR_Y, d); }
    else { tick(nm, chars_for(avail, 34), hl); text(x, ry, 34, VCR_W, nm); }
  }
  int hx = 40;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { text(hx, H - 50, 24, VCR_W, p->hints[i]); hx += tt_text_width(24, p->hints[i]) + 34; }
}
static void vcr_banner(const Channel *c, int number) {
  ban_track(c);
  textf(40, 30, 48, VCR_W, "CH%03d", number);
  char t[16]; clock_str(t, sizeof t, "%H:%M");
  text(W - 40 - tt_text_width(48, t), 30, 48, VCR_W, t);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  char nm[160]; snprintf(nm, sizeof nm, "%s%s", is_fav(c->key) ? "* " : "", c->name); tick(nm, 30, 2);
  text(40, H - 120, 40, VCR_W, nm);
  if (cur) { char l[200]; snprintf(l, sizeof l, "\xe2\x96\xb6 %s", cur->title); tick(l, 40, 2); text(40, H - 66, 30, VCR_Y, l); }
}
static void vcr_toast(const char *m, int line) { text(W - 40 - tt_text_width(32, m), 96 + line * 42, 32, VCR_W, m); }

// ================================================================================================================
// Cable guide: a purple listings panel on the lower part of the screen, the live channel above it
// ================================================================================================================
static const SDL_Color CB_Y = {255, 230, 80, 255}, CB_W = {255, 255, 255, 255}, CB_C = {150, 220, 255, 255};
static void cable_page(const Page *p) {
  sel_track(p);
  if (!p->tv_on) draw_testcard();
  if (p->kind == PAGE_GUIDE) {
    GridStyle g = { rgb(40, 0, 90), rgb(70, 20, 140), rgb(30, 40, 150), rgb(200, 150, 0), CB_W, rgb(255, 80, 80), rgb(120, 60, 200) };
    guide_grid(p, 0, H / 3, W, H - H / 3, &g);
    box(0, H / 3 - 50, W, 50, 70, 20, 140, 235); text(20, H / 3 - 42, 32, CB_Y, p->title);
    return;
  }
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { EntryStyle e = { rgb(40, 0, 90), CB_Y, rgb(200, 150, 0), rgb(0, 0, 0), 240 }; entry_page(p, &e); return; }
  int py = H / 3 - 20;
  box(0, py, W, H - py, 40, 0, 90, 235);
  box(0, py, W, 56, 90, 30, 170, 255);
  text(20, py + 10, 36, CB_Y, p->title);
  char t[16]; clock_str(t, sizeof t, "%H:%M");
  text(W - 20 - tt_text_width(36, t), py + 10, 36, CB_W, t);
  int y = py + 70;
  if (p->help[0]) { text(20, y, 22, CB_C, p->help); y += 30; }
  if (p->kind == PAGE_LOADING) { text(20, y + 20, 32, CB_W, p->status); return; }
  int rows = (H - y - 50) / 50;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * 50, hl = i == p->cursor;
    box(10, ry, W - 20, 46, hl ? 200 : ((i % 2) ? 55 : 65), hl ? 150 : 15, hl ? 0 : ((i % 2) ? 120 : 130), 255);
    int x = 20;
    if (p->kind == PAGE_LIST) { textf(x, ry + 9, 30, hl ? rgb(0, 0, 0) : CB_Y, "%3d", r->number); x += tt_text_width(30, "000 "); }
    if (p->kind == PAGE_CHECK) { text(x, ry + 9, 30, hl ? rgb(0, 0, 0) : CB_Y, r->checked ? "\xe2\x9c\x93" : "-"); x += 40; }
    if (p->kind == PAGE_LIST) {
      if (r->logo) { int lw, lh; SDL_QueryTexture(r->logo, NULL, NULL, &lw, &lh); double k = lw * 38 > lh * 54 ? 54.0 / lw : 38.0 / lh; SDL_Rect d = { x + (54 - (int)(lw * k)) / 2, ry + 4 + (38 - (int)(lh * k)) / 2, (int)(lw * k), (int)(lh * k) }; SDL_RenderCopy(ren, r->logo, NULL, &d); }
      x += 62;
    }
    if (p->kind == PAGE_LIST) { if (r->fav) text(x, ry + 9, 30, hl ? rgb(0, 0, 0) : CB_Y, "*"); x += tt_text_width(30, "*") + 8; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int avail = W - 30 - x;
    SDL_Color fg = hl ? rgb(0, 0, 0) : CB_W;
    if (r->detail[0] && strlen(r->detail) <= 8) { int dw = tt_text_width(26, r->detail); tick(nm, chars_for(avail - dw - 20, 30), hl); text(x, ry + 9, 30, fg, nm); text(W - 20 - dw, ry + 11, 26, hl ? rgb(40, 0, 60) : CB_C, r->detail); }
    else if (r->detail[0]) { tick(nm, chars_for(avail * 2 / 5, 30), hl); text(x, ry + 9, 30, fg, nm); char d[200]; snprintf(d, sizeof d, "%s", r->detail); tick(d, chars_for(avail * 3 / 5 - 10, 24), hl); text(x + avail * 2 / 5 + 10, ry + 12, 24, hl ? rgb(40, 0, 60) : CB_C, d); }
    else { tick(nm, chars_for(avail, 30), hl); text(x, ry + 9, 30, fg, nm); }
  }
  int hx = 20;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { text(hx, H - 36, 22, CB_Y, p->hints[i]); hx += tt_text_width(22, p->hints[i]) + 30; }
}
static void cable_banner(const Channel *c, int number) {
  ban_track(c);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  box(0, H - 110, W, 110, 40, 0, 90, 230); box(0, H - 110, W, 6, 255, 200, 40, 255);
  char nm[160]; snprintf(nm, sizeof nm, "%d  %s%s", number, is_fav(c->key) ? "* " : "", c->name); tick(nm, 32, 2);
  text(20, H - 94, 36, CB_Y, nm);
  char t1[8], t2[8], l[200];
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "NOW %s-%s %s", t1, t2, cur->title); tick(l, 50, 2); text(20, H - 48, 24, CB_W, l); }
}
static void cable_toast(const char *m, int line) { box(W - 30 - tt_text_width(28, m), 20 + line * 40, tt_text_width(28, m) + 20, 36, 90, 30, 170, 235); text(W - 20 - tt_text_width(28, m), 24 + line * 40, 28, CB_Y, m); }

// ================================================================================================================
// Game looks, shared: little sprites drawn from strings ('.' clear, '1'..'4' palette), word wrap, the battery
// ================================================================================================================
static void sprite(const char *const *rows, int nrows, int x, int y, int sc, const SDL_Color *pal) {
  for (int r = 0; r < nrows; r++) for (int c = 0; rows[r][c]; c++) {
    int k = rows[r][c] - '1'; if (k < 0 || k > 3) continue;
    box(x + c * sc, y + r * sc, sc, sc, pal[k].r, pal[k].g, pal[k].b, 255);
  }
}
#define SPRITE(rows, x, y, sc, pal) sprite(rows, (int)(sizeof rows / sizeof *rows), x, y, sc, pal)
static const char *const SPR_HEART[] = { ".11.11.", "1111111", "1111111", ".11111.", "..111..", "...1..." };
static const char *const SPR_HEART_HALF[] = { ".11.22.", "1112222", "1112222", ".11222.", "..122..", "...2..." };
static const char *const SPR_SWORD[] = { "..2.........", "11233333333.", "112333333334", "11233333333.", "..2........." };
static const char *const SPR_GEM[] = { "..1..", ".121.", "12221", "12221", "12221", "12221", ".121.", "..1.." };
static const char *const SPR_CLOCK[] = { ".111.", "12221", "12121", "12111", "12221", ".111." };
static const char *const SPR_HAND[] = { "..111111......", ".1222222111111", "12222222222221", "12222222111111",
                                        "1222222221....", "122222221.....", "1222222221....", ".122222221....", "..11111111...." };
static const char *const SPR_STAR[] = { "...1...", "...1...", "1111111", ".11111.", "..111..", ".11.11.", "1.....1" };
static const char *const SPR_CHECK[] = { "......1", ".....11", "1...11.", "11.11..", ".111...", "..1...." };
static const char *const SPR_ARROW[] = { "1....", "11...", "111..", "1111.", "111..", "11...", "1...." };
static const char *const SPR_TUFT[] = { "1...1", "1.1.1", ".111." };

// Battery level 0..100 from the power supply (the Brick has one), -1 when there is none; read every 30 s.
static int battery(void) {
  static int pct = -1; static Uint32 at; static int tried;
  if (tried && SDL_GetTicks() - at < 30000) return pct;
  tried = 1; at = SDL_GetTicks(); pct = -1;
  DIR *d = opendir("/sys/class/power_supply"); if (!d) return pct;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char path[512], type[32] = ""; snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
    FILE *f = fopen(path, "r"); if (!f) continue; if (!fgets(type, sizeof type, f)) type[0] = 0; fclose(f);
    if (strncmp(type, "Battery", 7)) continue;
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
    if ((f = fopen(path, "r"))) { int v; if (fscanf(f, "%d", &v) == 1) pct = v < 0 ? 0 : v > 100 ? 100 : v; fclose(f); }
    break;
  }
  closedir(d);
  return pct;
}
// Writes s in lines of at most `chars` characters, breaking at spaces; returns the y after the last line.
static int wrap(int x, int y, int h, int pitch, int chars, int max_lines, SDL_Color c, const char *s) {
  char buf[256]; snprintf(buf, sizeof buf, "%s", s);
  char *p = buf;
  for (int line = 0; *p && line < max_lines; line++) {
    while (*p == ' ') p++;
    int n = (int)strlen(p);
    if (n > chars) { int cut = chars; while (cut > 0 && p[cut] != ' ') cut--; if (cut == 0) cut = chars; char keep = p[cut]; p[cut] = 0; text(x, y, h, c, p); p[cut] = keep; p += cut; }
    else { text(x, y, h, c, p); p += n; }
    y += pitch;
  }
  return y;
}
// "A Watch" -> "A" and "Watch"
static void split_hint(const char *h, char *key, size_t kn, char *word, size_t wn) {
  const char *sp = strchr(h, ' ');
  if (!sp) { snprintf(key, kn, "%s", h); word[0] = 0; return; }
  snprintf(key, kn, "%.*s", (int)(sp - h), h); snprintf(word, wn, "%s", sp + 1);
}
static void upper(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
// Copies a row's detail to d; returns whether it is a short value for the right edge (a count in a menu, not a programme).
static int row_detail(const Page *p, const Row *r, char *d, size_t n) { snprintf(d, n, "%s", r->detail); return p->kind != PAGE_LIST && strlen(r->detail) <= 8; }

// ================================================================================================================
// 8-bit Quest: a top-down adventure's black HUD (map of the list's pages, gem counter, item boxes, LIFE hearts
// that show the battery) over a dungeon-walled panel; a sword points at the row. Original drawings, no game's art.
// ================================================================================================================
static const SDL_Color Q_W = {252, 252, 252, 255}, Q_RED = {216, 40, 0, 255}, Q_TAN = {252, 216, 168, 255},
  Q_OR = {252, 152, 56, 255}, Q_BLUE = {0, 88, 248, 255}, Q_GRAY = {116, 116, 116, 255}, Q_GREEN = {0, 200, 60, 255};
static const SDL_Color Q_SWORD_PAL[] = { {136, 76, 0, 255}, {252, 188, 60, 255}, {200, 220, 252, 255}, {252, 252, 252, 255} };
static const SDL_Color Q_HEART_PAL[] = { {216, 40, 0, 255}, {80, 20, 20, 255} };
static const SDL_Color Q_GEM_PAL[] = { {0, 88, 248, 255}, {120, 200, 252, 255} };
static const SDL_Color Q_CLOCK_PAL[] = { {252, 216, 168, 255}, {0, 0, 0, 255} };

static void q_wall(int x, int y, int w, int h, int t) {   // a brick wall frame t pixels thick
  box(x, y, w, h, 172, 80, 16, 255);
  for (int by = y; by < y + h; by += 12) {
    box(x, by, w, 2, 90, 40, 8, 255);
    int off = ((by - y) / 12) % 2 ? 12 : 0;
    for (int bx = x + off; bx < x + w; bx += 24) { box(bx, by, 2, 12, 90, 40, 8, 255); box(bx + 2, by + 2, 20, 2, 228, 130, 60, 255); }
  }
  box(x + t, y + t, w - 2 * t, h - 2 * t, 0, 0, 0, 255);
}
static void q_label_on_wall(int cx, int y, const char *s) {   // a cut-out in the wall with the text in it
  char b[64]; snprintf(b, sizeof b, " %s ", s); upper(b); fit(b, 36);
  int w = tw(24, b); box(cx - w / 2, y, w, 24, 0, 0, 0, 255); text(cx - w / 2, y, 24, Q_W, b);
}
static void q_hud(const Page *p, int rows_per_page) {
  // map: one room per page of the list, the current one blinking
  int mx = 32, my = 40, mw = 232, mh = 96;
  box(mx, my, mw, mh, Q_GRAY.r, Q_GRAY.g, Q_GRAY.b, 255);
  int pages = p->nrows ? (p->nrows + rows_per_page - 1) / rows_per_page : 1, cur = p->cursor / (rows_per_page ? rows_per_page : 1);
  if (pages > 32) { cur = cur * 32 / pages; pages = 32; }
  int cols = pages < 8 ? pages : 8, rws = (pages + 7) / 8;
  int cw = 26, ch = 18, ox = mx + (mw - cols * cw) / 2, oy = my + (mh - rws * ch) / 2;
  for (int i = 0; i < pages; i++) {
    int x = ox + (i % 8) * cw, y = oy + (i / 8) * ch;
    box(x + 2, y + 2, cw - 4, ch - 4, 0, 60, 160, 255);
    if (i == cur && (SDL_GetTicks() / 300) % 2) box(x + cw / 2 - 4, y + ch / 2 - 4, 8, 8, Q_GREEN.r, Q_GREEN.g, Q_GREEN.b, 255);
  }
  char pg[16]; snprintf(pg, sizeof pg, "PAGE-%d", p->page_no ? p->page_no : 1);
  text(mx, 12, 16, Q_W, pg);
  // counters
  SPRITE(SPR_GEM, 300, 44, 4, Q_GEM_PAL); textf(328, 52, 24, Q_W, "x%d", p->nrows);
  SPRITE(SPR_CLOCK, 296, 96, 5, Q_CLOCK_PAL); char t[8]; clock_str(t, sizeof t, "%H:%M"); text(328, 100, 24, Q_W, t);
  // item boxes: B and A
  for (int i = 0; i < 2; i++) {
    const char *h = p->hints[i == 0 ? 1 : 0]; if (!h || !h[0]) continue;
    char key[16], word[32]; split_hint(h, key, sizeof key, word, sizeof word); upper(word); fit(word, 6);
    int bx = 488 + i * 136, by = 40, bw = 116, bh = 84;
    box(bx, by, bw, bh, Q_BLUE.r, Q_BLUE.g, Q_BLUE.b, 255); box(bx + 6, by + 6, bw - 12, bh - 12, 0, 0, 0, 255);
    int kw = tw(16, key); box(bx + (bw - kw) / 2 - 4, by - 8, kw + 8, 16, 0, 0, 0, 255); text(bx + (bw - kw) / 2, by - 8, 16, Q_W, key);
    text(bx + (bw - tw(16, word)) / 2, by + 36, 16, Q_TAN, word);
  }
  // LIFE: the battery as 8 hearts (full when there is no battery)
  text(800, 30, 24, Q_RED, "-LIFE-");
  int pct = battery(); int halves = pct < 0 ? 16 : (pct * 16 + 50) / 100; if (pct >= 0 && halves < 1) halves = 1;
  for (int i = 0; i < 8; i++) {
    int x = 760 + i * 30, y = 76;
    if (halves >= 2 * (i + 1)) SPRITE(SPR_HEART, x, y, 3, Q_HEART_PAL);
    else if (halves == 2 * i + 1) SPRITE(SPR_HEART_HALF, x, y, 3, Q_HEART_PAL);
    else { static const SDL_Color e[] = { {80, 20, 20, 255} }; SPRITE(SPR_HEART, x, y, 3, e); }
  }
}
static void q_hints(const Page *p, int y) {   // the hints the item boxes don't hold, on the bottom wall
  char b[96] = ""; for (int i = 2; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { if (b[0]) strcat(b, "  "); strncat(b, p->hints[i], 30); }
  if (b[0]) q_label_on_wall(W / 2 - 150, y, b);
  if (p->status[0] && p->kind == PAGE_LIST) q_label_on_wall(W - 170, y, p->status);
}
static void quest_page_(const Page *p) {
  sel_track(p);
  box(0, 0, W, H, 0, 0, 0, 255);
  int rows = 11;
  q_hud(p, rows);
  int px = 16, py = 152, pw = W - 32, ph = H - 152 - 8, t = 24;
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { EntryStyle e = { rgb(0, 0, 0), Q_W, Q_BLUE, Q_W, 255 }; q_wall(px, py, pw, ph, t); entry_page(p, &e); return; }
  q_wall(px, py, pw, ph, t);
  q_label_on_wall(W / 2, py, p->title);
  if (p->kind == PAGE_GUIDE) {
    GridStyle g = { rgb(0, 0, 0), rgb(0, 60, 160), rgb(90, 40, 8), rgb(172, 80, 16), Q_W, Q_GREEN, Q_BLUE };
    guide_grid(p, px + t + 8, py + t + 12, pw - 2 * t - 16, ph - 2 * t - 24, &g);
    q_hints(p, py + ph - t);
    return;
  }
  int x0 = px + t + 16, y = py + t + 14;
  if (p->help[0]) { char h[160]; snprintf(h, sizeof h, "%s", p->help); fit(h, 54); text(x0, y, 16, Q_TAN, h); }
  y += 30;
  if (p->kind == PAGE_LOADING) {
    text(x0 + 40, y + 60, 32, Q_W, "PLEASE WAIT");
    char st[128]; snprintf(st, sizeof st, "%s", p->status); fit(st, 52); text(x0 + 40, y + 130, 16, Q_TAN, st);
    int span = pw - 2 * t - 140, sx = x0 + 40 + (int)((SDL_GetTicks() / 8) % (Uint32)span);
    SPRITE(SPR_SWORD, sx, y + 200, 6, Q_SWORD_PAL);
    return;
  }
  int row_h = 42, right = px + pw - t - 16;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * row_h, hl = i == p->cursor;
    if (hl) SPRITE(SPR_SWORD, x0 - 4 + ((SDL_GetTicks() / 250) % 2) * 4, ry + 4, 3, Q_SWORD_PAL);
    int x = x0 + 48;
    SDL_Color fg = hl ? Q_OR : Q_W;
    if (p->kind == PAGE_LIST) { textf(x, ry, 24, hl ? Q_OR : Q_GRAY, "%3d", r->number); x += tw(24, "000 "); }
    if (p->kind == PAGE_CHECK) { if (r->checked) SPRITE(SPR_HEART, x, ry + 3, 3, Q_HEART_PAL); else { static const SDL_Color e[] = { {80, 20, 20, 255} }; SPRITE(SPR_HEART, x, ry + 3, 3, e); } x += 36; }
    if (r->fav) { SPRITE(SPR_HEART, x, ry + 6, 2, Q_HEART_PAL); x += 22; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int avail = right - x;
    if (r->detail[0]) {
      char d[200];
      if (row_detail(p, r, d, sizeof d)) { int dw = tw(24, d); tick(nm, chars_for(avail - dw - 24, 24), hl); text(x, ry, 24, fg, nm); text(right - dw, ry, 24, hl ? Q_OR : Q_TAN, d); }
      else { tick(nm, chars_for(avail * 9 / 20, 24), hl); text(x, ry, 24, fg, nm); tick(d, chars_for(avail * 11 / 20 - 16, 16), hl); text(x + avail * 9 / 20 + 16, ry + 4, 16, hl ? Q_OR : Q_TAN, d); }
    } else { tick(nm, chars_for(avail, 24), hl); text(x, ry, 24, fg, nm); }
  }
  if (p->nrows > rows && p->top + rows < p->nrows && (SDL_GetTicks() / 400) % 2) text(right - 24, y + rows * row_h - 20, 16, Q_W, "v");
  q_hints(p, py + ph - t);
}
static void quest_page(const Page *p) { pfont = 1; quest_page_(p); pfont = 0; }
static void quest_banner(const Channel *c, int number) {
  ban_track(c);
  pfont = 1;
  int bx = 32, by = H - 196, bw = W - 64, bh = 172;
  box(bx, by, bw, bh, 0, 0, 0, 255); frame(bx + 6, by + 6, bw - 12, bh - 12, 4, Q_W);
  char nm[160]; snprintf(nm, sizeof nm, "CH-%02d %s", number, c->name); tick(nm, 25, 2);
  text(bx + 32, by + 28, 32, Q_W, nm);
  if (is_fav(c->key)) SPRITE(SPR_HEART, bx + bw - 64, by + 30, 4, Q_HEART_PAL);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  char t1[8], t2[8], l[200];
  if (cur) {
    strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop));
    snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); tick(l, 36, 2);
    // the title types itself out, like a game's text box
    int shown = (int)((SDL_GetTicks() - ban_t) / 35); char part[200]; snprintf(part, sizeof part, "%s", l); fit(part, shown);
    text(bx + 32, by + 80, 24, Q_TAN, part);
  }
  if (next) { strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "%s %s", t1, next->title); fit(l, 54); text(bx + 32, by + 122, 16, Q_OR, l); }
  pfont = 0;
}
static void quest_toast(const char *m, int line) {
  pfont = 1; char b[96]; snprintf(b, sizeof b, "%s", m); upper(b);
  int w = tw(24, b) + 32, x = W - 32 - w, y = 24 + line * 52;
  box(x, y, w, 44, 0, 0, 0, 255); frame(x + 3, y + 3, w - 6, 38, 3, Q_W); text(x + 16, y + 10, 24, Q_W, b);
  pfont = 0;
}

// ================================================================================================================
// 16-bit RPG: blue gradient windows with white rounded borders and a pointing hand, over the live picture (or a
// little field of grass); the banner is a status window whose bar is the time left of the programme.
// ================================================================================================================
static const SDL_Color R_W = {248, 248, 248, 255}, R_DIM = {176, 184, 248, 255}, R_Y = {248, 224, 96, 255}, R_GRAY = {150, 150, 170, 255};
static const SDL_Color R_HAND_PAL[] = { {40, 40, 56, 255}, {248, 248, 248, 255} };
static const SDL_Color R_STAR_PAL[] = { {248, 216, 64, 255} };
static void r_window(int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2) {   // gradient, corners cut round
    int in = dy < 2 || dy >= h - 2 ? 6 : dy < 4 || dy >= h - 4 ? 3 : dy < 6 || dy >= h - 6 ? 1 : 0;
    int k = dy * 255 / (h > 1 ? h : 1);
    box(x + in, y + dy, w - 2 * in, 2, 64 - k * 48 / 255, 64 - k * 48 / 255, 200 - k * 104 / 255, 240);
  }
  SDL_Color o = rgb(30, 30, 48), b = R_W, s = rgb(150, 150, 176);
  box(x + 8, y, w - 16, 2, o.r, o.g, o.b, 255); box(x + 8, y + h - 2, w - 16, 2, o.r, o.g, o.b, 255);
  box(x, y + 8, 2, h - 16, o.r, o.g, o.b, 255); box(x + w - 2, y + 8, 2, h - 16, o.r, o.g, o.b, 255);
  box(x + 8, y + 2, w - 16, 4, b.r, b.g, b.b, 255); box(x + 8, y + h - 6, w - 16, 4, b.r, b.g, b.b, 255);
  box(x + 2, y + 8, 4, h - 16, b.r, b.g, b.b, 255); box(x + w - 6, y + 8, 4, h - 16, b.r, b.g, b.b, 255);
  int cx[2] = { x + 2, x + w - 10 }, cy[2] = { y + 2, y + h - 10 };
  for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
    box(cx[i] + 2, cy[j] + (j ? 0 : 2), 4, 6, b.r, b.g, b.b, 255); box(cx[i] + (i ? 0 : 2), cy[j] + 2, 6, 4, b.r, b.g, b.b, 255);
  }
  box(x + 8, y + 6, w - 16, 2, s.r, s.g, s.b, 255); box(x + 6, y + 8, 2, h - 16, s.r, s.g, s.b, 255);
}
static void r_text(int x, int y, int h, SDL_Color c, const char *s) { int sc = pix_scale(h); text(x + sc / 2 + 1, y + sc / 2 + 1, h, rgb(16, 16, 40), s); text(x, y, h, c, s); }
static void r_field(void) {   // when no channel is on: grass, a path and a pond, in big pixels
  for (int y = 0; y < H; y += 32) for (int x = 0; x < W; x += 32) {
    int v = ((x / 32) * 7 + (y / 32) * 13) % 5;
    box(x, y, 32, 32, 56 + v * 4, 152 + v * 6, 56, 255);
    if (v == 0) { static const SDL_Color t[] = { {24, 104, 40, 255} }; SPRITE(SPR_TUFT, x + 8, y + 12, 3, t); }
  }
  box(0, 448, W, 64, 200, 168, 104, 255); box(0, 448, W, 4, 160, 128, 72, 255); box(0, 508, W, 4, 160, 128, 72, 255);
  box(760, 560, 224, 160, 48, 104, 216, 255); box(776, 576, 64, 8, 160, 200, 248, 255); box(880, 640, 48, 8, 160, 200, 248, 255);
}
static void rpg_page_(const Page *p) {
  sel_track(p);
  if (!p->tv_on) r_field();
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { r_window(64, 60, W - 128, H - 120); EntryStyle e = { rgb(32, 32, 140), R_W, rgb(96, 96, 232), R_Y, 255 }; entry_page(p, &e); return; }
  if (p->kind == PAGE_GUIDE) {
    r_window(16, 16, W - 32, H - 32); r_text(40, 34, 32, R_W, p->title);
    GridStyle g = { rgb(20, 20, 90), rgb(40, 40, 150), rgb(48, 48, 168), rgb(96, 96, 232), R_W, R_Y, rgb(120, 120, 248) };
    guide_grid(p, 36, 84, W - 72, H - 116, &g);
    return;
  }
  int lx = 16, lw = 648;
  // title
  r_window(lx, 16, lw, 72); char ti[64]; snprintf(ti, sizeof ti, "%s", p->title); fit(ti, 24); r_text(lx + 28, 36, 32, R_W, ti);
  // right column: time and place, commands, help
  int rx = lx + lw + 12, rw = W - 16 - rx;
  r_window(rx, 16, rw, 132);
  char t[8]; clock_str(t, sizeof t, "%H:%M");
  r_text(rx + 24, 36, 16, R_DIM, "TIME"); r_text(rx + rw - 24 - tw(32, t), 52, 32, R_W, t);
  if (p->kind == PAGE_LIST || p->kind == PAGE_GUIDE) { r_text(rx + 24, 100, 16, R_DIM, "NO."); r_text(rx + rw - 24 - tw(16, p->status), 100, 16, R_W, p->status); }
  else { char n[16]; snprintf(n, sizeof n, "%d", p->nrows); r_text(rx + 24, 100, 16, R_DIM, "ITEMS"); r_text(rx + rw - 24 - tw(16, n), 100, 16, R_W, n); }
  int nh = 0; for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) nh++;
  int cy = 160;
  if (nh) {
    r_window(rx, cy, rw, 32 + nh * 40);
    int k = 0;
    for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) {
      char key[16], word[32]; split_hint(p->hints[i], key, sizeof key, word, sizeof word); fit(word, 10);
      r_text(rx + 24, cy + 22 + k * 40, 24, R_Y, key); r_text(rx + 24 + tw(24, key) + 16, cy + 22 + k * 40, 24, R_W, word); k++;
    }
    cy += 32 + nh * 40 + 12;
  }
  if (p->help[0] && cy < H - 120) { r_window(rx, cy, rw, H - 16 - cy); wrap(rx + 20, cy + 22, 16, 24, (rw - 40) / 16, (H - 16 - cy - 40) / 24, R_W, p->help); }
  // the list
  int ly = 100, lh = H - 16 - ly;
  r_window(lx, ly, lw, lh);
  if (p->kind == PAGE_LOADING) {
    r_text(lx + 40, ly + 60, 32, R_W, "Now loading");
    static const char *dots[] = { "", ".", "..", "..." }; r_text(lx + 40 + tw(32, "Now loading"), ly + 60, 32, R_W, dots[(SDL_GetTicks() / 400) % 4]);
    wrap(lx + 40, ly + 130, 16, 26, (lw - 80) / 16, 6, R_DIM, p->status);
    return;
  }
  int rows = 11, row_h = 50, y = ly + 24, right = lx + lw - 28;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * row_h, hl = i == p->cursor;
    if (hl) { int bob = (int)(2 * sin(SDL_GetTicks() / 120.0)); SPRITE(SPR_HAND, lx + 12 + bob, ry + 2, 3, R_HAND_PAL); }
    int x = lx + 64;
    if (p->kind == PAGE_LIST) { char n[8]; snprintf(n, sizeof n, "%3d", r->number); r_text(x, ry + 4, 24, R_DIM, n); x += tw(24, "000 "); }
    if (p->kind == PAGE_CHECK) { if (r->checked) { static const SDL_Color c[] = { {248, 224, 96, 255} }; SPRITE(SPR_CHECK, x, ry + 8, 3, c); } else box(x + 6, ry + 14, 9, 9, R_GRAY.r, R_GRAY.g, R_GRAY.b, 255); x += 34; }
    if (r->fav) { SPRITE(SPR_STAR, x, ry + 8, 3, R_STAR_PAL); x += 28; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int avail = right - x;
    if (r->detail[0]) {
      char d[200];
      if (row_detail(p, r, d, sizeof d)) { int dw = tw(24, d); tick(nm, chars_for(avail - dw - 24, 24), hl); r_text(x, ry + 4, 24, R_W, nm); r_text(right - dw, ry + 4, 24, R_DIM, d); }
      else { tick(nm, chars_for(avail / 2, 24), hl); r_text(x, ry + 4, 24, R_W, nm); tick(d, chars_for(avail / 2 - 16, 16), hl); r_text(x + avail / 2 + 16, ry + 8, 16, R_DIM, d); }
    } else { tick(nm, chars_for(avail, 24), hl); r_text(x, ry + 4, 24, R_W, nm); }
  }
  if (p->top + rows < p->nrows) { int b = (SDL_GetTicks() / 300) % 2 * 3; static const char *const dn[] = { "11111", ".111.", "..1.." }; SPRITE(dn, right - 12, ly + lh - 30 + b, 3, R_STAR_PAL); }
}
static void rpg_page(const Page *p) { pfont = 1; rpg_page_(p); pfont = 0; }
static void rpg_banner(const Channel *c, int number) {
  ban_track(c);
  pfont = 1;
  int bx = 16, bh = 196, by = H - 16 - bh, bw = W - 32;
  r_window(bx, by, bw, bh);
  char n[8]; snprintf(n, sizeof n, "%d", number);
  char nm[160]; snprintf(nm, sizeof nm, "%s", c->name); tick(nm, (bw - 56 - 72 - tw(32, n) - 24) / 32, 2);
  r_text(bx + 28, by + 24, 32, R_Y, n); r_text(bx + 28 + tw(32, n) + 24, by + 24, 32, R_W, nm);
  if (is_fav(c->key)) SPRITE(SPR_STAR, bx + bw - 56, by + 26, 4, R_STAR_PAL);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  char l[200], t1[8];
  if (cur) {
    snprintf(l, sizeof l, "%s", cur->title); tick(l, 38, 2); r_text(bx + 28, by + 74, 24, R_W, l);
    // the programme's time left as a status bar: green, then yellow, then red
    time_t now = time(NULL); int total = (int)((cur->stop - cur->start) / 60), left = (int)((cur->stop - now + 59) / 60);
    if (total < 1) total = 1;
    if (left < 0) left = 0;
    if (left > total) left = total;
    r_text(bx + 28, by + 116, 16, R_DIM, "MIN");
    int gx = bx + 92, gw = 420, fill = gw * left / total;
    box(gx - 2, by + 112, gw + 4, 24, 16, 16, 40, 255);
    SDL_Color gc = left * 2 > total ? rgb(64, 216, 96) : left * 4 > total ? R_Y : rgb(232, 64, 48);
    box(gx, by + 114, fill, 20, gc.r, gc.g, gc.b, 255); box(gx, by + 114, fill, 4, 255, 255, 255, 120);
    char v[24]; snprintf(v, sizeof v, "%d/%d", left, total); r_text(gx + gw + 20, by + 114, 24, R_W, v);
  }
  if (next) { strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "NEXT %s %s", t1, next->title); fit(l, 56); r_text(bx + 28, by + 156, 16, R_DIM, l); }
  pfont = 0;
}
static void rpg_toast(const char *m, int line) {
  pfont = 1; int w = tw(24, m) + 48, x = W - 16 - w, y = 16 + line * 64;
  r_window(x, y, w, 56); r_text(x + 24, y + 16, 24, R_W, m);
  pfont = 0;
}

// ================================================================================================================
// Pocket: an old green handheld's four-shade dot-matrix screen, with its faint pixel grid
// ================================================================================================================
static const SDL_Color P0 = {15, 56, 15, 255}, P1 = {48, 98, 48, 255}, P2 = {139, 172, 15, 255}, P3 = {155, 188, 15, 255};
static void p_grid(int x, int y, int w, int h) {   // the LCD's pixel grid
  for (int gx = x; gx < x + w; gx += 4) box(gx, y, 1, h, P1.r, P1.g, P1.b, 28);
  for (int gy = y; gy < y + h; gy += 4) box(x, gy, w, 1, P1.r, P1.g, P1.b, 28);
}
static void p_box(int x, int y, int w, int h) {   // a text box: light, with a dark double border
  box(x, y, w, h, P3.r, P3.g, P3.b, 255); frame(x, y, w, h, 4, P0); frame(x + 8, y + 8, w - 16, h - 16, 2, P1);
}
static void pocket_page_(const Page *p) {
  sel_track(p);
  box(0, 0, W, H, P3.r, P3.g, P3.b, 255);
  if (p->kind == PAGE_PIN || p->kind == PAGE_KEYS) { EntryStyle e = { P3, P0, P0, P3, 255, P2 }; entry_page(p, &e); p_grid(0, 0, W, H); return; }
  box(0, 0, W, 64, P0.r, P0.g, P0.b, 255);
  char ti[64]; snprintf(ti, sizeof ti, "%s", p->title); upper(ti); fit(ti, 24); text(24, 16, 32, P3, ti);
  char t[8]; clock_str(t, sizeof t, "%H:%M"); text(W - 24 - tw(32, t), 16, 32, P2, t);
  if (p->kind == PAGE_GUIDE) {
    GridStyle g = { P3, P2, rgb(178, 204, 70), P2, P0, P0, rgb(110, 150, 30) };
    guide_grid(p, 0, 72, W, H - 72, &g); p_grid(0, 0, W, H); return;
  }
  int y = 80;
  if (p->help[0]) { char h[160]; snprintf(h, sizeof h, "%s", p->help); fit(h, 62); text(24, y, 16, P1, h); }
  y += 28;
  if (p->kind == PAGE_LOADING) {
    p_box(112, 220, W - 224, 300);
    text(W / 2 - tw(32, "LOADING") / 2, 280, 32, P0, "LOADING");
    int seg = (SDL_GetTicks() / 150) % 13;
    for (int i = 0; i < 12; i++) box(200 + i * 52, 350, 44, 32, i < seg ? P0.r : P2.r, i < seg ? P0.g : P2.g, i < seg ? P0.b : P2.b, 255);
    char st[128]; snprintf(st, sizeof st, "%s", p->status); fit(st, 44); text(W / 2 - tw(16, st) / 2, 430, 16, P1, st);
    p_grid(0, 0, W, H); return;
  }
  int rows = 12, row_h = 50, right = W - 56;
  for (int i = p->top; i < p->nrows && i < p->top + rows; i++) {
    const Row *r = &p->rows[i]; int ry = y + (i - p->top) * row_h, hl = i == p->cursor;
    if (hl) { box(16, ry - 6, right - 4, row_h - 4, P0.r, P0.g, P0.b, 255); SPRITE(SPR_ARROW, 24, ry + 2, 4, (&P3)); }
    SDL_Color fg = hl ? P3 : P0, dim = hl ? P2 : P1;
    int x = 56;
    if (p->kind == PAGE_LIST) { textf(x, ry, 32, dim, "%3d", r->number); x += tw(32, "000 "); }
    if (p->kind == PAGE_CHECK) { frame(x, ry + 2, 28, 28, 4, fg); if (r->checked) box(x + 8, ry + 10, 12, 12, fg.r, fg.g, fg.b, 255); x += 44; }
    if (r->fav) { SPRITE(SPR_HEART, x, ry + 8, 3, (hl ? &P2 : &P1)); x += 28; }
    char nm[200]; snprintf(nm, sizeof nm, "%s", r->text);
    int avail = right - 12 - x;
    if (r->detail[0]) {
      char d[200];
      if (row_detail(p, r, d, sizeof d)) { int dw = tw(24, d); tick(nm, chars_for(avail - dw - 24, 32), hl); text(x, ry, 32, fg, nm); text(right - 12 - dw, ry + 4, 24, dim, d); }
      else { tick(nm, chars_for(avail / 2, 32), hl); text(x, ry, 32, fg, nm); tick(d, chars_for(avail / 2 - 16, 16), hl); text(x + avail / 2 + 16, ry + 8, 16, dim, d); }
    } else { tick(nm, chars_for(avail, 32), hl); text(x, ry, 32, fg, nm); }
  }
  if (p->nrows > rows) {   // scroll bar
    int ty = y - 6, th = rows * row_h; box(W - 36, ty, 16, th, P2.r, P2.g, P2.b, 255);
    int kh = th * rows / p->nrows; if (kh < 24) kh = 24;
    box(W - 36, ty + (th - kh) * p->cursor / (p->nrows > 1 ? p->nrows - 1 : 1), 16, kh, P0.r, P0.g, P0.b, 255);
  }
  box(0, H - 48, W, 48, P2.r, P2.g, P2.b, 255);
  int hx = 24;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) {
    char key[16], word[32]; split_hint(p->hints[i], key, sizeof key, word, sizeof word); upper(word);
    int kw = tw(16, key); box(hx, H - 38, kw + 12, 28, P0.r, P0.g, P0.b, 255); text(hx + 6, H - 32, 16, P3, key);
    text(hx + kw + 20, H - 32, 16, P0, word); hx += kw + 20 + tw(16, word) + 28;
  }
  if (p->status[0] && p->kind == PAGE_LIST) text(W - 24 - tw(16, p->status), H - 32, 16, P0, p->status);
  p_grid(0, 0, W, H);
}
static void pocket_page(const Page *p) { pfont = 1; pocket_page_(p); pfont = 0; }
static void pocket_banner(const Channel *c, int number) {
  ban_track(c);
  pfont = 1;
  int bx = 16, bh = 176, by = H - 16 - bh, bw = W - 32;
  p_box(bx, by, bw, bh);
  char nm[160]; snprintf(nm, sizeof nm, "%s", c->name); tick(nm, 25, 2); text(bx + 32, by + 28, 32, P0, nm);
  char n[8]; snprintf(n, sizeof n, "%d", number);
  int nw = tw(32, n) + 24; box(bx + bw - 24 - nw, by + 22, nw, 44, P0.r, P0.g, P0.b, 255); text(bx + bw - 12 - nw, by + 28, 32, P3, n);
  if (is_fav(c->key)) SPRITE(SPR_HEART, bx + bw - 60 - nw, by + 32, 4, (&P1));
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  char t1[8], t2[8], l[200];
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); tick(l, 37, 2); text(bx + 32, by + 84, 24, P0, l); }
  if (next) { strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "%s %s", t1, next->title); fit(l, 55); text(bx + 32, by + 124, 16, P1, l); }
  if ((SDL_GetTicks() / 400) % 2) { static const char *const dn[] = { "11111", ".111.", "..1.." }; SPRITE(dn, bx + bw - 48, by + bh - 36, 4, (&P0)); }
  p_grid(bx, by, bw, bh);
  pfont = 0;
}
static void pocket_toast(const char *m, int line) {
  pfont = 1; char b[96]; snprintf(b, sizeof b, "%s", m); upper(b);
  int w = tw(24, b) + 48, x = W - 16 - w, y = 16 + line * 64;
  p_box(x, y, w, 56); text(x + 24, y + 16, 24, P0, b);
  pfont = 0;
}

static const Theme T_CRT = { "CRT TV", 10, 1, crt_page, crt_banner, crt_toast };
static const Theme T_TEXT = { "Teletext", 18, 0, tx_page, tx_banner, tx_toast };
static const Theme T_VCR = { "VCR", 12, 0, vcr_page, vcr_banner, vcr_toast };
static const Theme T_CABLE = { "Cable guide", 8, 1, cable_page, cable_banner, cable_toast };
static const Theme T_QUEST = { "8-bit Quest", 11, 0, quest_page, quest_banner, quest_toast };
static const Theme T_RPG = { "16-bit RPG", 11, 1, rpg_page, rpg_banner, rpg_toast };
static const Theme T_POCKET = { "Pocket", 12, 0, pocket_page, pocket_banner, pocket_toast };
const Theme *THEMES[] = { &T_CRT, &T_TEXT, &T_VCR, &T_CABLE, &T_QUEST, &T_RPG, &T_POCKET };
const int NTHEMES = 7;
