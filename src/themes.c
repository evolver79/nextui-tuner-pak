// Tuner's look: Dot (ui/dot), shared with Kino and Musicdisk. Menus and lists are glass cards on Tuner's evening
// ground (TUNER_THEME), or over the live channel (the picture under a 70% scrim, Dot's Night scheme). The accent is the
// one colour: the channel that is on. The TV engine's own screens (no signal, searching, retrying) and the stats are drawn here too.
#include "ui.h"
#include "tv.h"
#include "store.h"
#include "dot.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 1024
#define H 768
#define ROWS 5          // list rows at the PITCH: 96 + 5 rows = 624, inside the content band (96-640)
#define PITCH (DOT_ROW_H + 12)
// Tuner's ground and accent: broadcast blue (ground hi, mid, lo; accent). The one place to change Tuner's colours.
static const DotTheme TUNER_THEME = DOT_THEME_TUNER;   // teal
#define ACCENT TUNER_THEME.accent
static SDL_Renderer *ren;
static Dot *D;
static int still;       // screenshots: no blinking

int themes_init(SDL_Renderer *r, const char *pak_dir) {
  ren = r;
  char p[700]; snprintf(p, sizeof p, "%s/fonts", pak_dir);
  if (!(D = dot_init(r, p, ACCENT))) { fprintf(stderr, "fonts in %s: %s\n", p, SDL_GetError()); return -1; }
  dot_set_theme(D, &TUNER_THEME);
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

// s shortened with "…" (at a character boundary) so it is at most maxw wide in that style.
static void fit(char *out, size_t n, int style, const char *s, int maxw) {
  snprintf(out, n, "%s", s);
  size_t len = strlen(out);
  while (len > 1 && dot_text_w(D, style, out) > maxw) {
    do len--; while (len > 1 && ((unsigned char)s[len] & 0xc0) == 0x80);
    snprintf(out, n, "%.*s…", (int)len, s);
  }
}

// Text over up to `lines` lines of maxw, broken at spaces (the last line cut with "…"); returns the lines used.
// draw 0: only counts them.
static int wrap(int draw, int style, const char *s, int x, int y, Uint32 rgb, int maxw, int lines) {
  int lh = dot_line_h(D, style), used = 0;
  while (*s && used < lines) {
    while (*s == ' ') s++;
    size_t best = 0;
    if (used < lines - 1)
      for (size_t i = 0; ; i++) {   // the longest run up to a space that fits
        if (s[i] == ' ' || !s[i]) {
          char part[256]; snprintf(part, sizeof part, "%.*s", (int)i, s);
          if (dot_text_w(D, style, part) > maxw) break;
          best = i;
          if (!s[i]) break;
        }
      }
    if (!best) { if (draw) dot_text(D, style, s, x, y + used * lh, rgb, DOT_LEFT, maxw); return used + 1; }
    char part[256]; snprintf(part, sizeof part, "%.*s", (int)best, s);
    if (draw) dot_text(D, style, part, x, y + used * lh, rgb, DOT_LEFT, maxw);
    s += best; used++;
  }
  return used;
}
static int text_wrap(int style, const char *s, int x, int y, Uint32 rgb, int maxw, int lines) {
  return wrap(1, style, s, x, y, rgb, maxw, lines);
}

// A card with a title and a line or two under it, 30 px inside.
static void message_card(const char *title, const char *body) {
  dot_card(D, 40, 96, 944, 186);
  dot_text(D, DOT_TITLE, title, 70, 126, DOT_TEXT, DOT_LEFT, 884);
  if (body && *body) text_wrap(DOT_BODY_S, body, 70, 180, DOT_LABEL, 884, 2);
}

// A channel's logo in the row's icon well (DOT_WELL, 56 px), when it has arrived; the TV icon until then. The well is
// painted over the icon (solid, then the glass well's light tint), and the logo fits 48 x 40 inside its circle.
static void logo_in_well(SDL_Texture *t, int y, int sel) {
  int cx = 40 + 16 + DOT_WELL / 2, cy = y + DOT_ROW_H / 2;
  dot_circle(D, cx, cy, DOT_WELL / 2, sel ? DOT_INK : DOT_RAISED);
  if (!sel) dot_rect(D, cx - DOT_WELL / 2, cy - DOT_WELL / 2, DOT_WELL, DOT_WELL, DOT_WELL / 2, 0xffffff, 26);
  int w, h; if (SDL_QueryTexture(t, NULL, NULL, &w, &h) != 0 || w <= 0 || h <= 0) return;
  double k = 48.0 / w < 40.0 / h ? 48.0 / w : 40.0 / h;
  SDL_Rect d = { cx - (int)(w * k) / 2, cy - (int)(h * k) / 2, (int)(w * k), (int)(h * k) };
  SDL_RenderCopy(ren, t, NULL, &d);
}

static void list_rows(const Page *p) {
  int top = view_top(p, ROWS);
  const Channel *on = tv_state() != TV_OFF ? tv_channel() : NULL;
  for (int i = top; i < p->nrows && i < top + ROWS; i++) {
    const Row *r = &p->rows[i];
    int y = 96 + (i - top) * PITCH, sel = i == p->cursor;
    char num[12]; snprintf(num, sizeof num, "%d", r->number);
    int live = on && i < p->nchs && p->chs[i] == on;
    dot_row(D, 40, y, 944, DOT_I_TV, r->text, r->detail[0] ? r->detail : NULL, num,
            (sel ? DOT_FOCUS : 0) | (r->fav ? DOT_STAR : 0), live);
    if (r->logo) logo_in_well(r->logo, y, sel);
  }
}

// The guide: when the selected channel has a next programme, it shows in a pill in the fifth row's place (4 rows).
static void guide_rows(const Page *p) {
  time_t now = time(NULL);
  char l[200] = "";
  if (p->cursor >= 0 && p->cursor < p->nchs) {
    const Programme *cur, *next; now_next(p->chs[p->cursor], now, &cur, &next);
    if (next) { char t[8], s[200]; hhmm(t, sizeof t, next->start); snprintf(s, sizeof s, "Next %s · %.150s", t, next->title); fit(l, sizeof l, DOT_UI, s, 944 - 48); }
  }
  int rows = l[0] ? ROWS - 1 : ROWS, top = view_top(p, rows);
  const Channel *on = tv_state() != TV_OFF ? tv_channel() : NULL;
  for (int i = top; i < p->nchs && i < top + rows; i++) {
    Channel *c = p->chs[i];
    const Programme *cur, *next; now_next(c, now, &cur, &next);
    char sub[220] = "No guide", num[12], t1[8], t2[8];
    if (cur) { hhmm(t1, sizeof t1, cur->start); hhmm(t2, sizeof t2, cur->stop); snprintf(sub, sizeof sub, "%s–%s  %s", t1, t2, cur->title); }
    snprintf(num, sizeof num, "%d", i + 1);
    dot_row(D, 40, 96 + (i - top) * PITCH, 944, 0, c->name, sub, num,
            (i == p->cursor ? DOT_FOCUS : 0) | (is_fav(c->key) ? DOT_STAR : 0), on == c);
  }
  if (l[0]) dot_pill(D, 40, 96 + rows * PITCH + (DOT_ROW_H - DOT_PILL_H) / 2, l, 0, 0, DOT_LEFT);
}

static void menu_rows(const Page *p, int rows) {
  int top = view_top(p, rows);
  for (int i = top; i < p->nrows && i < top + rows; i++) {
    const Row *r = &p->rows[i];
    int y = 96 + (i - top) * PITCH, sel = i == p->cursor;
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
  dot_card(D, 40, 96, 944, 440);
  dot_text(D, DOT_TITLE, p->title, 70, 126, DOT_TEXT, DOT_LEFT, 884);
  int bw = 140, gap = 24, x0 = (W - (4 * bw + 3 * gap)) / 2;
  for (int i = 0; i < 4; i++) {
    int x = x0 + i * (bw + gap), sel = i == p->cursor;
    dot_rect(D, x, 230, bw, 180, 26, sel ? DOT_PAPER : DOT_RAISED, 255);
    char d[2] = { p->input[i] ? p->input[i] : '-', 0 };
    int lh = dot_line_h(D, DOT_DISPLAY_L);
    dot_text(D, DOT_DISPLAY_L, d, x + bw / 2, 230 + (180 - lh) / 2, sel ? DOT_INK : DOT_TEXT, DOT_CENTER, 0);
  }
  dot_text(D, DOT_BODY_S, "Up and down change the digit, left and right move.", 70, 470, DOT_LABEL, DOT_LEFT, 884);
  const char *h[4] = { "A OK", "<> Move", "B Cancel", NULL };
  dock_from(h);
}

static void keys_page(const Page *p) {
  dot_header(D, p->title, p->status);
  dot_card(D, 40, 96, 944, 518);   // 30 px inside: the field, then 4 rows of keys at a 92 pitch
  dot_rect(D, 70, 126, 884, 72, 26, DOT_RAISED, 255);   // the text typed so far
  char shown[80]; snprintf(shown, sizeof shown, "%s_", p->input);
  dot_text(D, DOT_VALUE_L, shown, 100, 162 - dot_line_h(D, DOT_VALUE_L) / 2, DOT_TEXT, DOT_LEFT, 830);
  int k = 0, row = 0, col = 0;
  for (const char *q = p->keys; *q; ) {
    if (*q == '|') { row++; col = 0; q++; continue; }
    char key[16]; int n = 0;
    if (*q == '<') { q++; while (*q && *q != '>' && n < 14) key[n++] = *q++; if (*q == '>') q++; }
    else key[n++] = *q++;
    key[n] = 0;
    int kw = n > 1 ? 111 : 81, x = 70 + col, y = 228 + row * 92, sel = k == p->cursor;   // 10 keys (882) or 6 + 3 wide (883) fill the 884 inside the card
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
  // 30 px inside the card: the state (dot and label), the message over up to two lines, then the dots
  const char *msg = p->status[0] ? p->status : "Finding channels";
  int lh = dot_line_h(D, DOT_BODY_S), lines = wrap(0, DOT_BODY_S, msg, 70, 176, DOT_LABEL, 884, 2);
  int dy = 176 + lines * lh + 24;
  dot_card(D, 40, 96, 944, dy + DOT_DOT_R + 30 - 96);
  text_wrap(DOT_BODY_S, msg, 70, 176, DOT_LABEL, 884, 2);
  dot_adot(D, 70, 126 + (dot_line_h(D, DOT_LABEL_T) - 14) / 2, dot_accent(D));
  dot_text(D, DOT_LABEL_T, p->title, 98, 126, DOT_TEXT, DOT_LEFT, 0);
  int lit = still ? 6 : (int)(SDL_GetTicks() / 120 % 50);   // 49 dots across the card, filling up while it works
  for (int i = 0; i < 49; i++) dot_circle(D, 70 + DOT_DOT_R + i * 18, dy, DOT_DOT_R, i < lit ? DOT_TEXT : DOT_TRACK);
}

static void dot_page(const Page *p) {
  dot_set_accent(D, ACCENT);
  ground(p->tv_on);
  if (p->kind == PAGE_LOADING) { loading_page(p); dot_set_night(D, 0); return; }
  if (p->kind == PAGE_PIN) { pin_page(p); dot_set_night(D, 0); return; }
  if (p->kind == PAGE_KEYS) { keys_page(p); dot_set_night(D, 0); return; }
  int front = p->page_no == 100, pill_rows = ROWS;
  if (front) {   // the front page: the name in dots, the clock and the counts
    char t[16], c[140]; clock_str(t, sizeof t, "%H:%M");
    dot_title(D, "TUNER");
    snprintf(c, sizeof c, "%s · %s", p->help, t);
    dot_header(D, NULL, c);
  } else {   // the title, and the count or help at the right; help that doesn't fit there goes in a pill below
    const char *right = p->kind == PAGE_LIST || p->kind == PAGE_GUIDE ? p->status : p->help;
    int fits = dot_text_w(D, DOT_LABEL_T, p->title) + dot_text_w(D, DOT_LABEL_T, right) + 60 <= 944;
    dot_header(D, p->title, fits ? right : NULL);
    if (!fits && right == p->help && p->nrows && p->help[0]) {   // under the rows, in the fifth row's place at most
      pill_rows = p->nrows < ROWS - 1 ? p->nrows : ROWS - 1;
      char l[200]; fit(l, sizeof l, DOT_UI, p->help, 944 - 48);
      dot_pill(D, 40, 96 + pill_rows * PITCH + (DOT_ROW_H - DOT_PILL_H) / 2, l, 0, 0, DOT_LEFT);
    }
  }
  if (!p->nrows && !(p->kind == PAGE_GUIDE && p->nchs)) message_card("Nothing here", p->help);
  else if (p->kind == PAGE_LIST) list_rows(p);
  else if (p->kind == PAGE_GUIDE) guide_rows(p);
  else menu_rows(p, pill_rows);
  dock_from(p->hints);
  dot_set_night(D, 0);
}

// Over the picture: the channel in a pill top left, its number and the time top right, and a card at the bottom
// with what's on now (how far it has got) and what's next.
static void dot_banner(const Channel *c, int number) {
  dot_set_accent(D, ACCENT);
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
  // The card sits 40 px off the bottom (no dock while watching); 30 px inside it: Now and the times, the title, the
  // progress, then what's next.
  int lab = dot_line_h(D, DOT_LABEL_T), tit = dot_line_h(D, DOT_TITLE), bod = dot_line_h(D, DOT_BODY_S);
  if (!cur) {
    int h = 30 + tit + 10 + bod + 26, y = H - 40 - h;
    dot_card(D, 40, y, 944, h);
    dot_text(D, DOT_TITLE, c->name, 70, y + 30, DOT_TEXT, DOT_LEFT, 884);
    dot_text(D, DOT_BODY_S, "No guide for this channel", 70, y + 30 + tit + 10, DOT_LABEL, DOT_LEFT, 884);
  } else {
    int h = 30 + lab + 12 + tit + 16 + 24 + 16 + bod + 26, y = H - 40 - h;
    dot_card(D, 40, y, 944, h);
    char t1[8], t2[8], l[220];
    hhmm(t1, sizeof t1, cur->start); hhmm(t2, sizeof t2, cur->stop);
    int ly = y + 30;
    dot_adot(D, 70, ly + (lab - 14) / 2, dot_accent(D));
    snprintf(l, sizeof l, "Now · %s–%s", t1, t2);
    dot_text(D, DOT_LABEL_T, l, 98, ly, DOT_TEXT, DOT_LEFT, 0);
    int fav = is_fav(c->key);
    if (fav) dot_icon(D, DOT_I_STAR, 954 - 13, ly + lab / 2, 26, DOT_TEXT);
    int ty = ly + lab + 12;
    dot_text(D, DOT_TITLE, cur->title, 70, ty, DOT_TEXT, DOT_LEFT, 884);
    long len2 = (long)(cur->stop - cur->start); if (len2 < 1) len2 = 1;
    int py = ty + tit + 16;
    dot_progress(D, 70, py, 884, (float)(now - cur->start) / len2, 0);
    if (next) { hhmm(t1, sizeof t1, next->start); snprintf(l, sizeof l, "Next %s · %s", t1, next->title); }
    else l[0] = 0;
    dot_text(D, DOT_BODY_S, l, 70, py + 24 + 16, DOT_LABEL, DOT_LEFT, 884);
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
  dot_set_accent(D, ACCENT);
  dot_ground(D);
  dot_header(D, "No signal", NULL);
  int dm = dot_line_h(D, DOT_DISPLAY_M), tit = dot_line_h(D, DOT_TITLE), bod = dot_line_h(D, DOT_BODY_S);
  int ty = 96 + 30 + dm + 18, ry = ty + tit + 8, lines = wrap(0, DOT_BODY_S, reason, 70, ry, DOT_LABEL, 884, 2);
  dot_card(D, 40, 96, 944, ry + lines * bod + 26 - 96);
  dot_text(D, DOT_DISPLAY_M, "NO SIGNAL", 70, 126, DOT_TEXT, DOT_LEFT, 884);
  dot_text(D, DOT_TITLE, name, 70, ty, DOT_TEXT, DOT_LEFT, 884);
  text_wrap(DOT_BODY_S, reason, 70, ry, DOT_LABEL, 884, 2);
  DotHint h[2] = { { "UP DOWN", "Another channel" }, { "B", "Back" } };
  dot_dock(D, h, 2);
}

void ui_notice(const char *msg) { dot_frame(D); dot_pill(D, W / 2, 36, msg, 1, 0, DOT_CENTER); }

// Stats for nerds (SELECT while watching): "LABEL|value" lines in a card.
void ui_nerd(char lines[][96], int n) {
  // 28 px type on a 40 px pitch, 30 px inside the card; labels (7 letters at most) in a 160 px column
  int lh = 40, th = dot_line_h(D, DOT_VALUE), h = 30 + (n - 1) * lh + th + 30, vx = 70 + 160;
  dot_rect(D, 40, 36, 944, h, DOT_R_CARD, DOT_CARD, 225);
  for (int i = 0; i < n; i++) {
    char l[96]; snprintf(l, sizeof l, "%s", lines[i]);
    char *bar = strchr(l, '|'); if (bar) *bar++ = 0;
    int y = 36 + 30 + i * lh;
    dot_text(D, DOT_LABEL_T, l, 70, y, DOT_LABEL, DOT_LEFT, vx - 70 - 12);
    if (bar) dot_text(D, DOT_VALUE, bar, vx, y, DOT_TEXT, DOT_LEFT, 954 - vx);
  }
}

static const Theme T_DOT = { "Dot", ROWS, 1, dot_page, dot_banner, dot_toast_, dot_volume };
const Theme *THEMES[] = { &T_DOT };
const int NTHEMES = 1;
