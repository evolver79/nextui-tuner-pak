// Tuner's look: Dot (ui/dot), shared with Kino and Musicdisk. Menus and lists are black cards on the grey ground,
// or over the live channel (the picture under a 70% scrim, Dot's Night scheme). Green is the one colour: the channel
// that is on. The TV engine's own screens (no signal, searching, retrying) and the stats are drawn here too.
#include "ui.h"
#include "tv.h"
#include "store.h"
#include "dot.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 1024
#define H 768
#define ROWS 5          // list rows at the 94 px pitch
static SDL_Renderer *ren;
static Dot *D;
static int still;       // screenshots: no blinking

int themes_init(SDL_Renderer *r, const char *pak_dir) {
  ren = r;
  char p[700]; snprintf(p, sizeof p, "%s/fonts", pak_dir);
  if (!(D = dot_init(r, p, DOT_ACCENT_TUNER))) { fprintf(stderr, "fonts in %s: %s\n", p, SDL_GetError()); return -1; }
  return 0;
}
void themes_still(int on) { still = on; }
void ui_frame(void) { dot_frame(D); }

static void clock_str(char *b, size_t n, const char *fmt) { time_t t = time(NULL); strftime(b, n, fmt, localtime(&t)); }
static void hhmm(char *b, size_t n, time_t t) { strftime(b, n, "%H:%M", localtime(&t)); }

// "A Watch" -> key "A", text "Watch"; "L2/R2" -> "L2 R2". Empty hints are left out.
static void dock_from(const char *const hints[4]) {
  static char keys[4][16]; DotHint h[4]; int n = 0;
  for (int i = 0; i < 4; i++) {
    if (!hints[i] || !hints[i][0]) continue;
    const char *sp = strchr(hints[i], ' ');
    snprintf(keys[n], sizeof keys[n], "%.*s", sp ? (int)(sp - hints[i]) : (int)strlen(hints[i]), hints[i]);
    for (char *q = keys[n]; *q; q++) if (*q == '/') *q = ' ';
    h[n] = (DotHint){ keys[n], sp ? sp + 1 : "" }; n++;
  }
  dot_dock(D, h, n);
}

// The ground: grey, or the channel playing behind the menu under the Night scrim.
static void ground(int tv_on) {
  dot_set_night(D, tv_on);
  if (tv_on) dot_scrim(D); else dot_ground(D);
}

static int view_top(const Page *p, int rows) {   // the first row shown, so the cursor is in view
  int t = p->top;
  if (p->cursor < t) t = p->cursor;
  if (p->cursor >= t + rows) t = p->cursor - rows + 1;
  return t < 0 ? 0 : t;
}

static void message_card(const char *title, const char *body) {
  dot_card(D, 40, 96, 944, 170);
  dot_text(D, DOT_TITLE, title, 76, 130, DOT_TEXT, DOT_LEFT, 872);
  if (body && *body) dot_text(D, DOT_BODY_S, body, 76, 184, DOT_LABEL, DOT_LEFT, 872);
}

// A channel's logo in the row's icon well (46 px), when it has arrived; the TV icon until then.
static void logo_in_well(SDL_Texture *t, int y, int sel) {
  int cx = 40 + 16 + DOT_WELL / 2, cy = y + DOT_ROW_H / 2;
  dot_circle(D, cx, cy, DOT_WELL / 2, sel ? DOT_INK : DOT_RAISED);
  int w, h; if (SDL_QueryTexture(t, NULL, NULL, &w, &h) != 0 || w <= 0 || h <= 0) return;
  double k = 32.0 / (w > h ? w : h);
  SDL_Rect d = { cx - (int)(w * k) / 2, cy - (int)(h * k) / 2, (int)(w * k), (int)(h * k) };
  SDL_RenderCopy(ren, t, NULL, &d);
}

static void list_rows(const Page *p) {
  int top = view_top(p, ROWS);
  const Channel *on = tv_state() != TV_OFF ? tv_channel() : NULL;
  for (int i = top; i < p->nrows && i < top + ROWS; i++) {
    const Row *r = &p->rows[i];
    int y = 96 + (i - top) * 94, sel = i == p->cursor;
    char num[12]; snprintf(num, sizeof num, "%d", r->number);
    int live = on && i < p->nchs && p->chs[i] == on;
    dot_row(D, 40, y, 944, DOT_I_TV, r->text, r->detail[0] ? r->detail : NULL, num,
            (sel ? DOT_FOCUS : 0) | (r->fav ? DOT_STAR : 0), live);
    if (r->logo) logo_in_well(r->logo, y, sel);
  }
}

static void guide_rows(const Page *p) {
  int top = view_top(p, ROWS);
  time_t now = time(NULL);
  const Channel *on = tv_state() != TV_OFF ? tv_channel() : NULL;
  for (int i = top; i < p->nchs && i < top + ROWS; i++) {
    Channel *c = p->chs[i];
    const Programme *cur, *next; now_next(c, now, &cur, &next);
    char sub[220] = "No guide", num[12], t1[8], t2[8];
    if (cur) { hhmm(t1, sizeof t1, cur->start); hhmm(t2, sizeof t2, cur->stop); snprintf(sub, sizeof sub, "%s–%s  %s", t1, t2, cur->title); }
    snprintf(num, sizeof num, "%d", i + 1);
    dot_row(D, 40, 96 + (i - top) * 94, 944, 0, c->name, sub, num,
            (i == p->cursor ? DOT_FOCUS : 0) | (is_fav(c->key) ? DOT_STAR : 0), on == c);
  }
  if (p->cursor >= 0 && p->cursor < p->nchs) {   // the selected channel's next programme, in the last slot
    const Programme *cur, *next; now_next(p->chs[p->cursor], now, &cur, &next);
    if (next) { char l[200], t[8]; hhmm(t, sizeof t, next->start); snprintf(l, sizeof l, "Next %s · %.150s", t, next->title); dot_pill(D, 40, 572, l, 0, 0, DOT_LEFT); }
  }
}

static void menu_rows(const Page *p) {
  int top = view_top(p, ROWS);
  for (int i = top; i < p->nrows && i < top + ROWS; i++) {
    const Row *r = &p->rows[i];
    int y = 96 + (i - top) * 94, sel = i == p->cursor;
    if (p->kind == PAGE_CHECK) {
      dot_row(D, 40, y, 944, 0, r->text, r->detail[0] ? r->detail : NULL, NULL, sel ? DOT_FOCUS : 0, 0);
      dot_toggle(D, 984 - 24 - DOT_TOGGLE_W, y + (DOT_ROW_H - DOT_TOGGLE_H) / 2, r->checked, sel);
    } else {   // short values at the right; longer ones as a second line
      int shortv = r->detail[0] && strlen(r->detail) <= 12;
      dot_row(D, 40, y, 944, 0, r->text, r->detail[0] && !shortv ? r->detail : NULL, shortv ? r->detail : NULL, sel ? DOT_FOCUS : 0, 0);
    }
  }
}

static void pin_page(const Page *p) {
  dot_header(D, p->title, NULL);
  dot_card(D, 40, 96, 944, 528);
  dot_text(D, DOT_TITLE, p->title, 76, 130, DOT_TEXT, DOT_LEFT, 872);
  int bw = 140, gap = 24, x0 = (W - (4 * bw + 3 * gap)) / 2;
  for (int i = 0; i < 4; i++) {
    int x = x0 + i * (bw + gap), sel = i == p->cursor;
    dot_rect(D, x, 230, bw, 180, 26, sel ? DOT_PAPER : DOT_RAISED, 255);
    char d[2] = { p->input[i] ? p->input[i] : '-', 0 };
    int lh = dot_line_h(D, DOT_DISPLAY_L);
    dot_text(D, DOT_DISPLAY_L, d, x + bw / 2, 230 + (180 - lh) / 2, sel ? DOT_INK : DOT_TEXT, DOT_CENTER, 0);
  }
  dot_text(D, DOT_BODY_S, "Up and down change the digit, left and right move.", 76, 470, DOT_LABEL, DOT_LEFT, 872);
  const char *h[4] = { "A OK", "<> Move", "B Cancel", NULL };
  dock_from(h);
}

static void keys_page(const Page *p) {
  dot_header(D, p->title, p->status);
  dot_card(D, 40, 96, 944, 506);
  dot_rect(D, 70, 120, 884, 72, 26, DOT_RAISED, 255);   // the text typed so far
  char shown[80]; snprintf(shown, sizeof shown, "%s_", p->input);
  dot_text(D, DOT_VALUE_L, shown, 100, 156 - dot_line_h(D, DOT_VALUE_L) / 2, DOT_TEXT, DOT_LEFT, 830);
  int k = 0, row = 0, col = 0;
  for (const char *q = p->keys; *q; ) {
    if (*q == '|') { row++; col = 0; q++; continue; }
    char key[16]; int n = 0;
    if (*q == '<') { q++; while (*q && *q != '>' && n < 14) key[n++] = *q++; if (*q == '>') q++; }
    else key[n++] = *q++;
    key[n] = 0;
    int kw = n > 1 ? 111 : 81, x = 70 + col, y = 222 + row * 92, sel = k == p->cursor;   // 10 keys (882) or 6 + 3 wide (883) fill the 884 inside the card
    dot_rect(D, x, y, kw, 80, 26, sel ? DOT_PAPER : DOT_RAISED, 255);
    int st = n > 1 ? DOT_LABEL_T : DOT_TITLE;
    dot_text(D, st, key, x + kw / 2, y + (80 - dot_line_h(D, st)) / 2, sel ? DOT_INK : DOT_TEXT, DOT_CENTER, 0);
    col += kw + 8; k++;
  }
  const char *h[4] = { "A Type", "Y Delete", "START Search", "B Back" };
  dock_from(h);
}

static void loading_page(const Page *p) {
  dot_title(D, "TUNER");
  char t[16]; clock_str(t, sizeof t, "%H:%M"); dot_header(D, NULL, t);
  dot_card(D, 40, 96, 944, 210);
  dot_adot(D, 76, 133, dot_accent(D));
  dot_text(D, DOT_LABEL_T, p->title, 104, 130, DOT_TEXT, DOT_LEFT, 0);
  dot_text(D, DOT_BODY_S, p->status[0] ? p->status : "Finding channels", 76, 176, DOT_LABEL, DOT_LEFT, 872);
  int lit = still ? 6 : (int)(SDL_GetTicks() / 120 % 49);   // dots filling up while it works
  for (int i = 0; i < 48; i++) dot_circle(D, 80 + i * 18, 256, DOT_DOT_R, i < lit ? DOT_TEXT : DOT_TRACK);
}

static void dot_page(const Page *p) {
  dot_set_accent(D, DOT_ACCENT_TUNER);
  ground(p->tv_on);
  if (p->kind == PAGE_LOADING) { loading_page(p); dot_set_night(D, 0); return; }
  if (p->kind == PAGE_PIN) { pin_page(p); dot_set_night(D, 0); return; }
  if (p->kind == PAGE_KEYS) { keys_page(p); dot_set_night(D, 0); return; }
  int front = p->page_no == 100;
  if (front) {   // the front page: the name in dots, the clock and the counts
    char t[16], c[140]; clock_str(t, sizeof t, "%H:%M");
    dot_title(D, "TUNER");
    snprintf(c, sizeof c, "%s · %s", p->help, t);
    dot_header(D, NULL, c);
  } else {   // the title, and the count or help at the right; help that doesn't fit there goes in a pill below
    const char *right = p->kind == PAGE_LIST || p->kind == PAGE_GUIDE ? p->status : p->help;
    int fits = dot_text_w(D, DOT_LABEL_T, p->title) + dot_text_w(D, DOT_LABEL_T, right) + 60 <= 944;
    dot_header(D, p->title, fits ? right : NULL);
    if (!fits && right == p->help && p->nrows) dot_pill(D, 40, 572, p->help, 0, 0, DOT_LEFT);
  }
  if (!p->nrows && !(p->kind == PAGE_GUIDE && p->nchs)) message_card("Nothing here", p->help);
  else if (p->kind == PAGE_LIST) list_rows(p);
  else if (p->kind == PAGE_GUIDE) guide_rows(p);
  else menu_rows(p);
  dock_from(p->hints);
  dot_set_night(D, 0);
}

// Over the picture: the channel in a pill top left, its number and the time top right, and a card at the bottom
// with what's on now (how far it has got) and what's next.
static void dot_banner(const Channel *c, int number) {
  dot_set_accent(D, DOT_ACCENT_TUNER);
  char t[16], b[48]; clock_str(t, sizeof t, "%H:%M");
  snprintf(b, sizeof b, "CH %d · %s", number, t);
  int bw = dot_text_w(D, DOT_PILL_MONO, b) + 48;
  char nm[110]; snprintf(nm, sizeof nm, "%s", c->name);
  size_t len = strlen(nm);   // shortened with "…" to leave room for the right pill
  while (len > 1 && dot_text_w(D, DOT_UI, nm) + 76 > 944 - bw - 24) {
    do len--; while (len > 1 && ((unsigned char)c->name[len] & 0xc0) == 0x80);
    snprintf(nm, sizeof nm, "%.*s…", (int)len, c->name);
  }
  dot_pill(D, 40, 36, nm, 1, 0, DOT_LEFT);
  dot_pill(D, 984, 36, b, 0, 1, DOT_RIGHT);
  time_t now = time(NULL);
  const Programme *cur, *next; now_next(c, now, &cur, &next);
  dot_card(D, 40, 532, 944, 200);
  if (!cur) {
    dot_text(D, DOT_TITLE, c->name, 76, 562, DOT_TEXT, DOT_LEFT, 872);
    dot_text(D, DOT_BODY_S, "No guide for this channel", 76, 612, DOT_LABEL, DOT_LEFT, 872);
  } else {
    char t1[8], t2[8], l[220];
    hhmm(t1, sizeof t1, cur->start); hhmm(t2, sizeof t2, cur->stop);
    dot_adot(D, 76, 565, dot_accent(D));
    snprintf(l, sizeof l, "Now · %s–%s", t1, t2);
    dot_text(D, DOT_LABEL_T, l, 104, 562, DOT_TEXT, DOT_LEFT, 0);
    if (is_fav(c->key)) dot_icon(D, DOT_I_STAR, 948 - 11, 573, 22, DOT_TEXT);
    dot_text(D, DOT_TITLE, cur->title, 76, 596, DOT_TEXT, DOT_LEFT, 872);
    long len2 = (long)(cur->stop - cur->start); if (len2 < 1) len2 = 1;
    dot_progress(D, 76, 648, 872, (float)(now - cur->start) / len2, 0);
    if (next) { hhmm(t1, sizeof t1, next->start); snprintf(l, sizeof l, "Next %s · %s", t1, next->title); }
    else l[0] = 0;
    dot_text(D, DOT_BODY_S, l, 76, 686, DOT_LABEL, DOT_LEFT, 872);
  }
}

static void dot_toast_(const char *m, int line) {
  if (line == 0) { dot_toast(D, m, 1); return; }
  dot_pill(D, W / 2, 120, m, 1, 0, DOT_CENTER);   // a second line: LOADING while a plain tuning screen waits
}

// The volume, when it changes: a pill at the top with a row of dots.
static void dot_volume(int v, int max, int muted) {
  int dots = 20, on = muted ? 0 : v * dots / (max > 0 ? max : 1);
  const char *label = muted ? "Muted" : "Volume";
  int lw = dot_text_w(D, DOT_UI, label), w = 48 + lw + 24 + dots * DOT_DOT_PITCH, x = (W - w) / 2;
  dot_rect(D, x, 36, w, DOT_PILL_H, DOT_PILL_H / 2, DOT_CARD, 219);
  dot_text(D, DOT_UI, label, x + 24, 36 + (DOT_PILL_H - dot_line_h(D, DOT_UI)) / 2, DOT_TEXT, DOT_LEFT, 0);
  for (int i = 0; i < dots; i++) dot_circle(D, x + 24 + lw + 24 + i * DOT_DOT_PITCH + DOT_DOT_R, 36 + DOT_PILL_H / 2, DOT_DOT_R, i < on ? DOT_TEXT : DOT_TRACK);
}

// The test card, shown on long waits: black, the time in dots, the name under it.
void draw_testcard(void) {
  SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
  char t[16]; clock_str(t, sizeof t, "%H:%M");
  int lh = dot_line_h(D, DOT_DISPLAY_XL), tw = dot_text_w(D, DOT_DISPLAY_XL, t);
  dot_time(D, DOT_DISPLAY_XL, t, (W - tw) / 2, H / 2 - lh, DOT_TEXT, DOT_LEFT);
  dot_text(D, DOT_LABEL_T, "Tuner", W / 2, H / 2 + 20, DOT_LABEL, DOT_CENTER, 0);
}

void ui_searching(void) {
  if (still || (SDL_GetTicks() / 600) % 2) dot_pill(D, W / 2, H - 120, "Searching for signal", 1, 0, DOT_CENTER);
}

void ui_no_signal(const char *name, const char *reason) {
  dot_set_accent(D, DOT_ACCENT_TUNER);
  dot_ground(D);
  dot_header(D, "No signal", NULL);
  dot_card(D, 40, 96, 944, 230);
  dot_text(D, DOT_DISPLAY_M, "NO SIGNAL", 76, 130, DOT_TEXT, DOT_LEFT, 872);
  dot_text(D, DOT_TITLE, name, 76, 200, DOT_TEXT, DOT_LEFT, 872);
  dot_text(D, DOT_BODY_S, reason, 76, 250, DOT_LABEL, DOT_LEFT, 872);
  DotHint h[2] = { { "UP DOWN", "Another channel" }, { "B", "Back" } };
  dot_dock(D, h, 2);
}

void ui_notice(const char *msg) { dot_frame(D); dot_pill(D, W / 2, 36, msg, 1, 0, DOT_CENTER); }

// Stats for nerds (SELECT while watching): "LABEL|value" lines in a card.
void ui_nerd(char lines[][96], int n) {
  int lh = 34, h = 48 + n * lh;
  dot_rect(D, 40, 36, 944, h, DOT_R_CARD, DOT_CARD, 225);
  for (int i = 0; i < n; i++) {
    char l[96]; snprintf(l, sizeof l, "%s", lines[i]);
    char *bar = strchr(l, '|'); if (bar) *bar++ = 0;
    int y = 60 + i * lh;
    dot_text(D, DOT_LABEL_T, l, 76, y, DOT_LABEL, DOT_LEFT, 170);
    if (bar) dot_text(D, DOT_VALUE, bar, 256, y, DOT_TEXT, DOT_LEFT, 700);
  }
}

static const Theme T_DOT = { "Dot", ROWS, 1, dot_page, dot_banner, dot_toast_, dot_volume };
const Theme *THEMES[] = { &T_DOT };
const int NTHEMES = 1;
