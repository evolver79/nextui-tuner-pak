// The looks: CRT TV (menus over the live picture), Teletext, VCR and Cable guide. They all draw the same Page.
#include "ui.h"
#include "tt.h"
#include "tv.h"
#include "store.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

#define W 1024
#define H 768
static SDL_Renderer *ren;
void themes_init(SDL_Renderer *r) { ren = r; }

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
static void text(int x, int y, int h, SDL_Color c, const char *s) { tt_draw(x, y, h, s, c); }
static void textf(int x, int y, int h, SDL_Color c, const char *fmt, ...) {
  char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); tt_draw(x, y, h, b, c);
}
static void fit(char *s, int chars) {   // shorten UTF-8 to `chars` characters
  int n = 0; for (char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) { if (n == chars) { *p = 0; return; } n++; }
}
static int chars_for(int width, int h) { return width / (h * 4 / 5); }
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
typedef struct { SDL_Color bg, fg, hl, hl_fg; int alpha; } EntryStyle;
static void entry_page(const Page *p, const EntryStyle *st) {
  int bw = 860, bh = p->kind == PAGE_PIN ? 300 : 520, bx = (W - bw) / 2, by = (H - bh) / 2;
  box(bx, by, bw, bh, st->bg.r, st->bg.g, st->bg.b, st->alpha);
  frame(bx, by, bw, bh, 4, st->fg);
  text(bx + 30, by + 24, 40, st->fg, p->title);
  if (p->help[0]) text(bx + 30, by + 76, 22, st->fg, p->help);
  if (p->kind == PAGE_PIN) {
    int dw = 90, x0 = (W - 4 * dw) / 2;
    for (int i = 0; i < 4; i++) {
      int hl = i == p->cursor, x = x0 + i * dw, y = by + 130;
      box(x, y, dw - 16, 110, hl ? st->hl.r : 0, hl ? st->hl.g : 0, hl ? st->hl.b : 0, 255);
      char d[2] = { p->input[i] ? p->input[i] : '-', 0 };
      text(x + (dw - 16 - tt_text_width(80, d)) / 2, y + 14, 80, hl ? st->hl_fg : st->fg, d);
    }
  } else {
    char shown[80]; snprintf(shown, sizeof shown, "%s_", p->input);
    box(bx + 30, by + 110, bw - 60, 56, 0, 0, 0, 255);
    text(bx + 44, by + 120, 36, st->fg, shown);
    int k = 0, row = 0, col = 0, kx0 = bx + 40, ky0 = by + 190;
    for (const char *q = p->keys; *q; ) {
      if (*q == '|') { row++; col = 0; q++; continue; }
      char key[16]; int n = 0;
      if (*q == '<') { q++; while (*q && *q != '>' && n < 14) key[n++] = *q++; if (*q == '>') q++; }
      else key[n++] = *q++;
      key[n] = 0;
      int kw = n > 1 ? 110 : 74, x = kx0 + col, y = ky0 + row * 74, hl = k == p->cursor;
      box(x, y, kw - 8, 64, hl ? st->hl.r : 30, hl ? st->hl.g : 30, hl ? st->hl.b : 30, 255);
      text(x + (kw - 8 - tt_text_width(n > 1 ? 22 : 40, key)) / 2, y + (n > 1 ? 20 : 12), n > 1 ? 22 : 40, hl ? st->hl_fg : st->fg, key);
      col += kw; k++;
    }
    if (p->status[0]) text(bx + 30, by + bh - 40, 22, st->fg, p->status);
  }
}

// ================================================================================================================
// CRT TV: a 90s TV set's on-screen menus, over the live picture (or a test card when nothing is tuned)
// ================================================================================================================
static const SDL_Color CRT_GREEN = {90, 255, 90, 255}, CRT_WHITE = {255, 255, 255, 255}, CRT_DIM = {150, 200, 150, 255};
static void crt_page(const Page *p) {
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
    char nm[200]; snprintf(nm, sizeof nm, "%s%s", r->text, r->fav ? " *" : "");
    int avail = bx + bw - 28 - x;
    if (r->detail[0] && p->kind != PAGE_CHECK && strlen(r->detail) <= 8) {   // a count or a short value: right edge
      int dw = tt_text_width(28, r->detail);
      fit(nm, chars_for(avail - dw - 20, 34)); text(x, ry + 4, 34, fg, nm);
      text(bx + bw - 28 - dw, ry + 8, 28, hl ? CRT_WHITE : CRT_DIM, r->detail);
    } else if (r->detail[0] && p->kind != PAGE_CHECK) {
      fit(nm, chars_for(avail / 2, 34)); text(x, ry + 4, 34, fg, nm);
      char d[200]; snprintf(d, sizeof d, "%s", r->detail); fit(d, chars_for(avail / 2 - 10, 24));
      text(x + avail / 2 + 10, ry + 10, 24, hl ? CRT_WHITE : CRT_DIM, d);
    } else { fit(nm, chars_for(avail, 34)); text(x, ry + 4, 34, fg, nm); }
  }
  if (p->nrows > rows) textf(bx + bw - 120, by + bh - 40, 22, CRT_DIM, "%d/%d", p->cursor + 1, p->nrows);
  int hx = bx + 28;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { text(hx, by + bh - 40, 22, CRT_WHITE, p->hints[i]); hx += tt_text_width(22, p->hints[i]) + 30; }
}
static void crt_banner(const Channel *c, int number) {
  char n[16]; snprintf(n, sizeof n, "%02d", number);
  text(W - 40 - tt_text_width(96, n), 30, 96, CRT_GREEN, n);   // the big green channel number of old TVs
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  box(0, H - 150, W, 150, 0, 0, 0, 160);
  char nm[160]; snprintf(nm, sizeof nm, "%s%s", c->name, is_fav(c->key) ? " *" : ""); fit(nm, 30);
  text(40, H - 134, 40, CRT_WHITE, nm);
  char t1[8], t2[8], l[200];
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); fit(l, 46); text(40, H - 84, 26, CRT_GREEN, l); }
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
    fit(nm, name_w);
    tt_print(col, row, hl ? TT_WHITE : (p->kind == PAGE_CHECK && r->checked ? TT_YELLOW : TT_WHITE), bg, 0, nm);
    if (r->detail[0]) { char d[200]; snprintf(d, sizeof d, "%s", r->detail); fit(d, TT_COLS - col - name_w - 1); tt_print(col + name_w + 1, row, hl ? TT_YELLOW : TT_CYAN, bg, 0, d); }
  }
  if (p->status[0]) tt_print(TT_COLS - (int)strlen(p->status), 23, TT_WHITE, TT_BLACK, 0, p->status);
  int cols[4] = { TT_RED, TT_GREEN, TT_YELLOW, TT_CYAN };
  for (int i = 0; i < 4; i++) if (p->hints[i]) tt_print(i * 10, 24, cols[i], TT_BLACK, 0, p->hints[i]);
  tt_render();
}
static void tx_banner(const Channel *c, int number) {
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  int h = 34 + (cur ? 34 : 0) + (next ? 34 : 0) + 16;
  box(0, H - h, W, h, 0, 0, 0, 255);
  char l[200]; snprintf(l, sizeof l, "P%03d %s%s", number, c->name, is_fav(c->key) ? " *" : ""); fit(l, 38);
  text(16, H - h + 8, 30, TT_PALETTE[TT_YELLOW], l);
  char t1[8], t2[8]; int y = H - h + 44;
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "%s-%s %s", t1, t2, cur->title); fit(l, 40); text(16, y, 28, TT_PALETTE[TT_WHITE], l); y += 34; }
  if (next) { strftime(t1, 8, "%H:%M", localtime(&next->start)); snprintf(l, sizeof l, "%s %s", t1, next->title); fit(l, 40); text(16, y, 28, TT_PALETTE[TT_CYAN], l); }
}
static void tx_toast(const char *m, int line) { box(W - 24 - tt_text_width(28, m) - 8, 22 + line * 40, tt_text_width(28, m) + 16, 36, 0, 0, 0, 255); text(W - 24 - tt_text_width(28, m), 26 + line * 40, 28, TT_PALETTE[TT_YELLOW], m); }

// ================================================================================================================
// VCR: the video recorder's blue screen with white blocky text
// ================================================================================================================
static const SDL_Color VCR_W = {255, 255, 255, 255}, VCR_Y = {255, 255, 120, 255};
static void vcr_page(const Page *p) {
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
    char nm[200]; snprintf(nm, sizeof nm, "%s%s", r->text, r->fav ? " *" : "");
    int avail = W - 40 - x;
    if (r->detail[0] && strlen(r->detail) <= 8) { int dw = tt_text_width(28, r->detail); fit(nm, chars_for(avail - dw - 20, 34)); text(x, ry, 34, VCR_W, nm); text(W - 40 - dw, ry + 4, 28, VCR_Y, r->detail); }
    else if (r->detail[0]) { fit(nm, chars_for(avail / 2, 34)); text(x, ry, 34, VCR_W, nm); char d[200]; snprintf(d, sizeof d, "%s", r->detail); fit(d, chars_for(avail / 2 - 10, 24)); text(x + avail / 2 + 10, ry + 6, 24, VCR_Y, d); }
    else { fit(nm, chars_for(avail, 34)); text(x, ry, 34, VCR_W, nm); }
  }
  int hx = 40;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { text(hx, H - 50, 24, VCR_W, p->hints[i]); hx += tt_text_width(24, p->hints[i]) + 34; }
}
static void vcr_banner(const Channel *c, int number) {
  textf(40, 30, 48, VCR_W, "CH%03d", number);
  char t[16]; clock_str(t, sizeof t, "%H:%M");
  text(W - 40 - tt_text_width(48, t), 30, 48, VCR_W, t);
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  char nm[160]; snprintf(nm, sizeof nm, "%s%s", c->name, is_fav(c->key) ? " *" : ""); fit(nm, 30);
  text(40, H - 120, 40, VCR_W, nm);
  if (cur) { char l[200]; snprintf(l, sizeof l, "\xe2\x96\xb6 %s", cur->title); fit(l, 40); text(40, H - 66, 30, VCR_Y, l); }
}
static void vcr_toast(const char *m, int line) { text(W - 40 - tt_text_width(32, m), 96 + line * 42, 32, VCR_W, m); }

// ================================================================================================================
// Cable guide: a purple listings panel on the lower part of the screen, the live channel above it
// ================================================================================================================
static const SDL_Color CB_Y = {255, 230, 80, 255}, CB_W = {255, 255, 255, 255}, CB_C = {150, 220, 255, 255};
static void cable_page(const Page *p) {
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
    char nm[200]; snprintf(nm, sizeof nm, "%s%s", r->text, r->fav ? " *" : "");
    int avail = W - 30 - x;
    SDL_Color fg = hl ? rgb(0, 0, 0) : CB_W;
    if (r->detail[0] && strlen(r->detail) <= 8) { int dw = tt_text_width(26, r->detail); fit(nm, chars_for(avail - dw - 20, 30)); text(x, ry + 9, 30, fg, nm); text(W - 20 - dw, ry + 11, 26, hl ? rgb(40, 0, 60) : CB_C, r->detail); }
    else if (r->detail[0]) { fit(nm, chars_for(avail * 2 / 5, 30)); text(x, ry + 9, 30, fg, nm); char d[200]; snprintf(d, sizeof d, "%s", r->detail); fit(d, chars_for(avail * 3 / 5 - 10, 24)); text(x + avail * 2 / 5 + 10, ry + 12, 24, hl ? rgb(40, 0, 60) : CB_C, d); }
    else { fit(nm, chars_for(avail, 30)); text(x, ry + 9, 30, fg, nm); }
  }
  int hx = 20;
  for (int i = 0; i < 4; i++) if (p->hints[i] && p->hints[i][0]) { text(hx, H - 36, 22, CB_Y, p->hints[i]); hx += tt_text_width(22, p->hints[i]) + 30; }
}
static void cable_banner(const Channel *c, int number) {
  const Programme *cur, *next; now_next(c, time(NULL), &cur, &next);
  box(0, H - 110, W, 110, 40, 0, 90, 230); box(0, H - 110, W, 6, 255, 200, 40, 255);
  char nm[160]; snprintf(nm, sizeof nm, "%d  %s%s", number, c->name, is_fav(c->key) ? " *" : ""); fit(nm, 32);
  text(20, H - 94, 36, CB_Y, nm);
  char t1[8], t2[8], l[200];
  if (cur) { strftime(t1, 8, "%H:%M", localtime(&cur->start)); strftime(t2, 8, "%H:%M", localtime(&cur->stop)); snprintf(l, sizeof l, "NOW %s-%s %s", t1, t2, cur->title); fit(l, 50); text(20, H - 48, 24, CB_W, l); }
}
static void cable_toast(const char *m, int line) { box(W - 30 - tt_text_width(28, m), 20 + line * 40, tt_text_width(28, m) + 20, 36, 90, 30, 170, 235); text(W - 20 - tt_text_width(28, m), 24 + line * 40, 28, CB_Y, m); }

static const Theme T_CRT = { "CRT TV", 10, 1, crt_page, crt_banner, crt_toast };
static const Theme T_TEXT = { "Teletext", 18, 0, tx_page, tx_banner, tx_toast };
static const Theme T_VCR = { "VCR", 12, 0, vcr_page, vcr_banner, vcr_toast };
static const Theme T_CABLE = { "Cable guide", 8, 1, cable_page, cable_banner, cable_toast };
const Theme *THEMES[] = { &T_CRT, &T_TEXT, &T_VCR, &T_CABLE };
const int NTHEMES = 4;
