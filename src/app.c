// Tuner – IPTV Player: free TV for NextUI. Pluto TV, Samsung TV Plus and iptv-org channels for the languages and
// countries chosen at first start, your own M3U playlists, favourites, a guide, and four looks (themes.c).
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "channels.h"
#include "sources.h"
#include "store.h"
#include "tt.h"
#include "tv.h"
#include "ui.h"
#include "logos.h"

// Pad buttons in the Brick's own names (it reports an Xbox layout with Nintendo labels).
enum { BTN_A = SDL_CONTROLLER_BUTTON_B, BTN_B = SDL_CONTROLLER_BUTTON_A, BTN_X = SDL_CONTROLLER_BUTTON_Y,
       BTN_Y = SDL_CONTROLLER_BUTTON_X, BTN_START = SDL_CONTROLLER_BUTTON_START, BTN_SELECT = SDL_CONTROLLER_BUTTON_BACK,
       BTN_MENU = SDL_CONTROLLER_BUTTON_GUIDE, BTN_L1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_R1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
       BTN_UP = SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_DOWN = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       BTN_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
       BTN_L2 = 100, BTN_R2 = 101 };   // the inner triggers are axes; turned into presses below

typedef enum { SCR_SETUP, SCR_LOADING, SCR_INDEX, SCR_GENRES, SCR_LIST, SCR_GUIDE, SCR_SETTINGS, SCR_EDIT, SCR_WATCH, SCR_PIN, SCR_SEARCH } Screen;
typedef enum { LIST_FAVS, LIST_GENRE, LIST_ALL, LIST_NOW, LIST_MINE, LIST_SEARCH } ListKind;
typedef enum { EDIT_LANG, EDIT_COUNTRY, EDIT_GENRE } EditKind;

static SDL_Renderer *ren;
static SDL_AudioDeviceID audio;
static Settings set;
static ChannelList chans;
static Channel **view; static int nview;          // the list on screen, and the zap list when watching
static Screen screen = SCR_INDEX, back_from_watch = SCR_LIST;
static ListKind list_kind; static int list_genre;
static EditKind edit_kind; static int setup_step;
static int cursor, top, watch_index;
static int dirty_sources, running = 1;
static Uint32 banner_until, toast_until, last_move;
static char toast[64];
static int show_osd;
static int prev_index = -1;                        // X while watching: back to the channel before
static Uint32 sleep_at; static int sleep_min, sleep_warned;
static int pin_unlocked, pin_setting, pin_digits[4], pin_pos;   // adult PIN: entering to set (pin_setting) or to unlock
static Screen pin_return;
static char query[48];                             // search text
static const char *KEYS = "ABCDEFGHIJ|KLMNOPQRST|UVWXYZ0123|456789<SPACE><DEL><OK>";
#define NKEYS 39
static const Theme *theme(void) { return THEMES[set.theme % NTHEMES]; }

// --- loading the channels in the background ----------------------------------------------------------------------
static SDL_mutex *load_lock; static char load_msg[128]; static volatile int load_done;
static ChannelList loaded;
static void progress(const char *msg, void *ud) { (void)ud; SDL_LockMutex(load_lock); snprintf(load_msg, sizeof load_msg, "%s", msg); SDL_UnlockMutex(load_lock); }
static int load_thread(void *arg) {
  ChannelList l = {0};
  sources_build(&set, &l, (int)(intptr_t)arg, progress, NULL);
  loaded = l; load_done = 1;
  return 0;
}
static void start_loading(int force) {
  load_done = 0; screen = SCR_LOADING; tv_tune(NULL);
  SDL_DetachThread(SDL_CreateThread(load_thread, "load", (void *)(intptr_t)force));
}

// --- views ---------------------------------------------------------------------------------------------------------
static int adult_locked(void) { return set.pin[0] && !pin_unlocked; }
static int visible(const Channel *c) { return !is_hidden(c->key) && !is_dead(c->key) && !(c->genre == G_ADULT && adult_locked()); }
static int contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle); if (!n) return 1;
  for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return 1;
  return 0;
}
static int is_mine(const Channel *c) { return !strcmp(c->source, "custom"); }
static int count_where(int (*f)(const Channel *, int), int arg) { int n = 0; for (int i = 0; i < chans.n; i++) if (visible(&chans.v[i]) && f(&chans.v[i], arg)) n++; return n; }
static int f_genre(const Channel *c, int g) { return c->genre == g; }
static int f_fav(const Channel *c, int g) { (void)g; return is_fav(c->key); }
static int f_all(const Channel *c, int g) { (void)c; (void)g; return 1; }
static int f_mine(const Channel *c, int g) { (void)g; return is_mine(c); }
static int has_adult(void) { for (int i = 0; i < chans.n; i++) if (chans.v[i].genre == G_ADULT && !is_hidden(chans.v[i].key)) return 1; return 0; }

static void build_view(void) {
  free(view); view = malloc((chans.n + 1) * sizeof *view); nview = 0;
  for (int i = 0; i < chans.n; i++) {
    Channel *c = &chans.v[i];
    if (!visible(c)) continue;
    if (list_kind == LIST_FAVS && !is_fav(c->key)) continue;
    if (list_kind == LIST_GENRE && c->genre != list_genre) continue;
    if (list_kind == LIST_NOW && !c->nprogs) continue;
    if (list_kind == LIST_MINE && !is_mine(c)) continue;
    if (list_kind == LIST_SEARCH && !contains_ci(c->name, query)) continue;
    view[nview++] = c;
  }
}
static void open_list(ListKind k, int genre, Screen scr) { list_kind = k; list_genre = genre; build_view(); cursor = top = 0; screen = scr; }
static void keep_visible(int n) {
  int rows = screen == SCR_GUIDE ? 8 : theme()->list_rows;
  if (cursor >= n) cursor = n - 1;
  if (cursor < 0) cursor = 0;
  if (cursor < top) top = cursor;
  if (cursor >= top + rows) top = cursor - rows + 1;
}
static void say(const char *m) { snprintf(toast, sizeof toast, "%s", m); toast_until = SDL_GetTicks() + 1500; }

// --- menus -----------------------------------------------------------------------------------------------------------
typedef struct { const char *label; int page; } MenuItem;
static const MenuItem INDEX_ITEMS[] = { {"Favourites", 101}, {"Channels by genre", 200}, {"All channels", 250},
                                        {"My channels", 260}, {"Guide", 300}, {"Search", 400}, {"Settings", 900} };
#define NINDEX 7
#define NSETTINGS 13
static void setting_row(int i, Row *r) {
  memset(r, 0, sizeof *r);
  switch (i) {
    case 0: snprintf(r->text, sizeof r->text, "Theme"); snprintf(r->detail, sizeof r->detail, "%s  (L2/R2)", theme()->name); break;
    case 1: snprintf(r->text, sizeof r->text, "Languages"); snprintf(r->detail, sizeof r->detail, "%s", set.languages); break;
    case 2: snprintf(r->text, sizeof r->text, "Countries"); snprintf(r->detail, sizeof r->detail, "%s", set.countries[0] ? set.countries : "-"); break;
    case 3: snprintf(r->text, sizeof r->text, "Genres"); break;
    case 4: snprintf(r->text, sizeof r->text, "Adult channels"); snprintf(r->detail, sizeof r->detail, "%s", set.adult ? "ON" : "OFF"); break;
    case 5: snprintf(r->text, sizeof r->text, "Max quality"); snprintf(r->detail, sizeof r->detail, "%dp", set.max_height); break;
    case 6: snprintf(r->text, sizeof r->text, "Picture"); snprintf(r->detail, sizeof r->detail, "%s  (L1/R1)", tv_overlay_name()); break;
    case 7: snprintf(r->text, sizeof r->text, "Refresh channels now"); break;
    case 8: snprintf(r->text, sizeof r->text, "Show hidden channels again"); break;
    case 9: snprintf(r->text, sizeof r->text, "Sleep timer"); if (sleep_at) snprintf(r->detail, sizeof r->detail, "%d min (%d left)", sleep_min, (int)((sleep_at - SDL_GetTicks()) / 60000) + 1); else snprintf(r->detail, sizeof r->detail, "OFF"); break;
    case 10: snprintf(r->text, sizeof r->text, "Adult PIN"); snprintf(r->detail, sizeof r->detail, "%s", set.pin[0] ? "SET (A: change, X: remove)" : "none"); break;
    case 11: snprintf(r->text, sizeof r->text, "Tuning screen"); snprintf(r->detail, sizeof r->detail, "%s", set.plain_tuning ? "Plain" : "Snow + picture"); break;
    case 12: snprintf(r->text, sizeof r->text, "Run first-time setup again"); break;
  }
}

// Multi-select pages (setup steps and the settings' lists)
static int edit_count(void) { return edit_kind == EDIT_LANG ? NLANGUAGES : edit_kind == EDIT_COUNTRY ? NCOUNTRIES : G_COUNT; }
static int edit_checked(int i) {
  if (edit_kind == EDIT_LANG) return list_has(set.languages, LANGUAGES[i].code);
  if (edit_kind == EDIT_COUNTRY) return list_has(set.countries, COUNTRIES[i].code);
  return i == G_ADULT ? set.adult : (int)((set.genres >> i) & 1);
}
static void edit_toggle(int i) {
  if (edit_kind == EDIT_LANG) list_toggle(set.languages, sizeof set.languages, LANGUAGES[i].code);
  else if (edit_kind == EDIT_COUNTRY) list_toggle(set.countries, sizeof set.countries, COUNTRIES[i].code);
  else if (i == G_ADULT) set.adult = !set.adult;
  else set.genres ^= 1u << i;
  dirty_sources = 1;
}

// --- building the Page for the theme -------------------------------------------------------------------------------
static Row rows_buf[4096];
static Page page;
static void fill_page(void) {
  Page *p = &page; memset(p, 0, sizeof *p);
  p->rows = rows_buf; p->cursor = cursor; p->top = top;
  p->tv_on = tv_state() != TV_OFF;
  time_t now = time(NULL);
  switch (screen) {
    case SCR_INDEX:
      p->kind = PAGE_MENU; p->page_no = 100; snprintf(p->title, sizeof p->title, "Tuner");
      snprintf(p->help, sizeof p->help, "%d channels, %d favourites", count_where(f_all, 0), count_where(f_fav, 0));
      for (int i = 0; i < NINDEX; i++) {
        Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
        r->number = INDEX_ITEMS[i].page; snprintf(r->text, sizeof r->text, "%s", INDEX_ITEMS[i].label);
        int n = i == 0 ? count_where(f_fav, 0) : i == 2 ? count_where(f_all, 0) : i == 3 ? count_where(f_mine, 0) : -1;
        if (n >= 0) snprintf(r->detail, sizeof r->detail, "%d", n);
      }
      p->hints[0] = "A Open"; p->hints[1] = "MENU Quit"; p->hints[2] = "L2/R2 Theme";
      break;
    case SCR_GENRES:
      p->kind = PAGE_MENU; p->page_no = 200; snprintf(p->title, sizeof p->title, "Channels by genre");
      for (int g = 0; g < G_COUNT; g++) {
        int n = count_where(f_genre, g); if (!n) continue;
        Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
        r->number = 201 + g; snprintf(r->text, sizeof r->text, "%s", GENRE_NAMES[g]); snprintf(r->detail, sizeof r->detail, "%d", n);
      }
      if (adult_locked() && has_adult()) { Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r); r->number = 201 + G_ADULT; snprintf(r->text, sizeof r->text, "Adult"); snprintf(r->detail, sizeof r->detail, "locked"); }
      p->hints[0] = "A Open"; p->hints[1] = "B Back";
      break;
    case SCR_LIST: case SCR_GUIDE: {
      static const char *names[] = { "Favourites", "", "All channels", "Guide", "My channels", "Search" };
      static const int pages[] = { 101, 0, 250, 300, 260, 401 };
      p->kind = screen == SCR_GUIDE ? PAGE_GUIDE : PAGE_LIST;
      p->page_no = list_kind == LIST_GENRE ? 201 + list_genre : pages[list_kind];
      if (list_kind == LIST_SEARCH) snprintf(p->title, sizeof p->title, "Search: %s", query);
      else snprintf(p->title, sizeof p->title, "%s", list_kind == LIST_GENRE ? GENRE_NAMES[list_genre] : names[list_kind]);
      p->chs = view; p->nchs = nview;
      for (int i = 0; i < nview && i < 4096; i++) {
        Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
        r->number = i + 1; snprintf(r->text, sizeof r->text, "%s", view[i]->name); r->fav = is_fav(view[i]->key);
        const Programme *cur, *next; now_next(view[i], now, &cur, &next);
        if (cur) snprintf(r->detail, sizeof r->detail, "%s", cur->title);
        if (screen == SCR_LIST && i >= top && i < top + theme()->list_rows) r->logo = logo_get(view[i]->logo);
      }
      if (!nview) snprintf(p->help, sizeof p->help, "%s", list_kind == LIST_FAVS ? "No favourites yet: press Y on a channel." :
                           list_kind == LIST_MINE ? "Add playlists to playlists.txt in Tuner.pak" : "Nothing here.");
      snprintf(p->status, sizeof p->status, "%d/%d", nview ? cursor + 1 : 0, nview);
      p->hints[0] = "A Watch"; p->hints[1] = "B Back"; p->hints[2] = "Y Fav"; p->hints[3] = screen == SCR_GUIDE ? "" : "X Hide";
      break;
    }
    case SCR_SETTINGS:
      p->kind = PAGE_MENU; p->page_no = 900; snprintf(p->title, sizeof p->title, "Settings");
      for (int i = 0; i < NSETTINGS; i++) setting_row(i, &p->rows[p->nrows++]);
      p->hints[0] = "A Change"; p->hints[1] = "B Back";
      break;
    case SCR_EDIT: case SCR_SETUP: {
      static const char *titles[] = { "Languages", "Countries", "Genres" };
      static const char *help[] = { "Channels in these languages (iptv-org).", "Free TV services and national channels.", "Genres to show. Adult is off unless set." };
      p->kind = PAGE_CHECK;
      p->page_no = screen == SCR_SETUP ? 800 + setup_step : 910 + edit_kind;
      if (screen == SCR_SETUP) snprintf(p->title, sizeof p->title, "Setup %d/3: %s", setup_step + 1, titles[edit_kind]);
      else snprintf(p->title, sizeof p->title, "%s", titles[edit_kind]);
      snprintf(p->help, sizeof p->help, "%s", help[edit_kind]);
      for (int i = 0; i < edit_count(); i++) {
        Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
        r->checked = edit_checked(i);
        if (edit_kind == EDIT_LANG) snprintf(r->text, sizeof r->text, "%s", LANGUAGES[i].name);
        else if (edit_kind == EDIT_COUNTRY) { snprintf(r->text, sizeof r->text, "%s", COUNTRIES[i].name); snprintf(r->detail, sizeof r->detail, "%s", country_services(COUNTRIES[i].code)); }
        else snprintf(r->text, sizeof r->text, "%s", GENRE_NAMES[i]);
      }
      p->hints[0] = "A Toggle"; p->hints[1] = "B Back"; p->hints[2] = screen == SCR_SETUP ? "START Next" : "";
      break;
    }
    case SCR_LOADING:
      p->kind = PAGE_LOADING; p->page_no = 199; snprintf(p->title, sizeof p->title, "Tuning in");
      SDL_LockMutex(load_lock); snprintf(p->status, sizeof p->status, "%s", load_msg); SDL_UnlockMutex(load_lock);
      break;
    case SCR_PIN:
      p->kind = PAGE_PIN; p->page_no = 990; p->cursor = pin_pos;
      snprintf(p->title, sizeof p->title, "%s", pin_setting ? "New adult PIN" : "Adult PIN");
      snprintf(p->help, sizeof p->help, "Up/down: digit, left/right: move, A: OK, B: cancel");
      for (int i = 0; i < 4; i++) p->input[i] = pin_setting || i == pin_pos ? (char)('0' + pin_digits[i]) : (i < pin_pos ? '*' : '-');
      break;
    case SCR_SEARCH: {
      p->kind = PAGE_KEYS; p->page_no = 400; p->keys = KEYS; p->cursor = cursor;
      snprintf(p->title, sizeof p->title, "Search");
      snprintf(p->input, sizeof p->input, "%s", query);
      ListKind k = list_kind; list_kind = LIST_SEARCH; build_view(); list_kind = k;
      snprintf(p->status, sizeof p->status, "%d channels match", nview);
      break;
    }
    case SCR_WATCH: break;
  }
}

// --- watching --------------------------------------------------------------------------------------------------------
static void tune_view(int i) {
  if (!nview) return;
  int next = (i + nview) % nview;
  if (screen == SCR_WATCH && next != watch_index) prev_index = watch_index;
  watch_index = next;
  if (strcmp(set.last_channel, view[watch_index]->key)) { snprintf(set.last_channel, sizeof set.last_channel, "%s", view[watch_index]->key); settings_save(&set); }
  tv_tune(view[watch_index]);
  banner_until = SDL_GetTicks() + 6000;
}
static void watch(int i) { back_from_watch = screen; prev_index = -1; tune_view(i); screen = SCR_WATCH; }
static void leave_watch(void) {
  screen = back_from_watch;
  if (!theme()->live_menus) tv_tune(NULL);   // themes that cover the screen stop the channel in the menus
  build_view();
  cursor = watch_index < nview ? watch_index : nview - 1; keep_visible(nview);
}

static void ask_pin(int setting, Screen ret) { pin_setting = setting; pin_return = ret; pin_pos = 0; memset(pin_digits, 0, sizeof pin_digits); screen = SCR_PIN; }
static void pin_done(void) {
  char entered[5]; for (int i = 0; i < 4; i++) entered[i] = (char)('0' + pin_digits[i]); entered[4] = 0;
  if (pin_setting) { snprintf(set.pin, sizeof set.pin, "%s", entered); settings_save(&set); pin_unlocked = 0; say("PIN SET"); screen = SCR_SETTINGS; return; }
  if (!strcmp(entered, set.pin)) { pin_unlocked = 1; say("UNLOCKED"); open_list(LIST_GENRE, G_ADULT, SCR_LIST); }
  else { say("WRONG PIN"); screen = pin_return; }
}

static void step_theme(int dir) {
  set.theme = (set.theme + dir + NTHEMES) % NTHEMES; settings_save(&set);
  if (!theme()->live_menus && screen != SCR_WATCH) tv_tune(NULL);
  char m[64]; snprintf(m, sizeof m, "THEME: %s", theme()->name); say(m);
  keep_visible(screen == SCR_LIST ? nview : 4096);
}

static void on_button(int b) {
  if (b == BTN_MENU) { running = 0; return; }
  if (b == BTN_L2 || b == BTN_R2) { step_theme(b == BTN_R2 ? 1 : -1); return; }
  int rows = screen == SCR_GUIDE ? 8 : theme()->list_rows;
  int page_step = (b == BTN_L1 || b == BTN_LEFT) ? -rows : (b == BTN_R1 || b == BTN_RIGHT) ? rows : 0;
  int step = b == BTN_UP ? -1 : b == BTN_DOWN ? 1 : 0;
  switch (screen) {
    case SCR_WATCH:
      if (b == BTN_UP || b == BTN_DOWN) tune_view(watch_index + (b == BTN_UP ? 1 : -1));
      if (b == BTN_A) banner_until = SDL_GetTicks() < banner_until ? 0 : 0xFFFFFFFFu;
      if (b == BTN_Y) { toggle_fav(view[watch_index]->key); say(is_fav(view[watch_index]->key) ? "* FAVOURITE" : "NOT A FAVOURITE"); }
      if (b == BTN_L1 || b == BTN_R1) { tv_overlay_step(b == BTN_R1 ? 1 : -1); say(tv_overlay_name()); }
      if (b == BTN_SELECT) show_osd = !show_osd;
      if (b == BTN_X && prev_index >= 0 && prev_index < nview) tune_view(prev_index);   // last channel
      if (b == BTN_B) leave_watch();
      break;
    case SCR_INDEX:
      if (step) cursor = (cursor + step + NINDEX) % NINDEX;
      if (b == BTN_A) {
        int c = cursor; cursor = top = 0;
        if (c == 0) open_list(LIST_FAVS, 0, SCR_LIST);
        else if (c == 1) screen = SCR_GENRES;
        else if (c == 2) open_list(LIST_ALL, 0, SCR_LIST);
        else if (c == 3) open_list(LIST_MINE, 0, SCR_LIST);
        else if (c == 4) open_list(LIST_NOW, 0, SCR_GUIDE);
        else if (c == 5) { screen = SCR_SEARCH; cursor = 0; }
        else screen = SCR_SETTINGS;
      }
      break;
    case SCR_GENRES: {
      int n = 0; for (int g = 0; g < G_COUNT; g++) if (count_where(f_genre, g)) n++;
      cursor += step + page_step; keep_visible(n);
      int locked_row = adult_locked() && has_adult();
      if (locked_row) { n++; keep_visible(n); }
      if (b == BTN_A && n) {
        if (locked_row && cursor == n - 1) ask_pin(0, SCR_GENRES);
        else { int r = 0; for (int g = 0; g < G_COUNT; g++) if (count_where(f_genre, g)) { if (r == cursor) { open_list(LIST_GENRE, g, SCR_LIST); break; } r++; } }
      }
      if (b == BTN_B) { screen = SCR_INDEX; cursor = 1; top = 0; }
      break;
    }
    case SCR_LIST: case SCR_GUIDE:
      cursor += step + page_step; keep_visible(nview);
      if (step || page_step) last_move = SDL_GetTicks();
      if (b == BTN_A) watch(cursor);
      if (b == BTN_Y && nview) { toggle_fav(view[cursor]->key); if (list_kind == LIST_FAVS) { build_view(); keep_visible(nview); } }
      if (b == BTN_X && nview && screen == SCR_LIST) { toggle_hidden(view[cursor]->key); say("HIDDEN"); build_view(); keep_visible(nview); }
      if (b == BTN_B) {
        if (list_kind == LIST_GENRE) { screen = SCR_GENRES; cursor = top = 0; }
        else if (list_kind == LIST_SEARCH) { screen = SCR_SEARCH; cursor = NKEYS - 1; top = 0; }
        else { screen = SCR_INDEX; cursor = list_kind == LIST_FAVS ? 0 : list_kind == LIST_ALL ? 2 : list_kind == LIST_MINE ? 3 : 4; top = 0; }
      }
      break;
    case SCR_SETTINGS:
      if (step) cursor = (cursor + step + NSETTINGS) % NSETTINGS;
      if (b == BTN_A || ((b == BTN_LEFT || b == BTN_RIGHT) && (cursor == 0 || cursor == 6 || cursor == 11))) {
        int dir = b == BTN_LEFT ? -1 : 1;
        if (cursor == 0) step_theme(dir);
        else if (cursor <= 3) { edit_kind = (EditKind)(cursor - 1); screen = SCR_EDIT; cursor = top = 0; }
        else if (cursor == 4) { set.adult = !set.adult; dirty_sources = 1; settings_save(&set); }
        else if (cursor == 5) { set.max_height = set.max_height == 480 ? 720 : set.max_height == 720 ? 1080 : 480; dirty_sources = 1; settings_save(&set); }
        else if (cursor == 6) tv_overlay_step(dir);
        else if (cursor == 7) { dirty_sources = 0; start_loading(1); }
        else if (cursor == 8) { unhide_all(); say("ALL CHANNELS SHOWN"); }
        else if (cursor == 9) {
          static const int steps[] = { 0, 30, 60, 90, 120 };
          int k = 0; while (k < 4 && steps[k] != sleep_min) k++;
          sleep_min = steps[(k + 1) % 5]; sleep_at = sleep_min ? SDL_GetTicks() + sleep_min * 60000u : 0; sleep_warned = 0;
        }
        else if (cursor == 10) ask_pin(1, SCR_SETTINGS);
        else if (cursor == 11) { set.plain_tuning = !set.plain_tuning; settings_save(&set); }
        else if (cursor == 12) { screen = SCR_SETUP; setup_step = 0; edit_kind = EDIT_LANG; cursor = top = 0; }
      }
      if (b == BTN_X && cursor == 10 && set.pin[0]) { set.pin[0] = 0; settings_save(&set); say("PIN REMOVED"); }
      if (b == BTN_B) { screen = SCR_INDEX; cursor = 6; top = 0; if (dirty_sources) { dirty_sources = 0; start_loading(0); } }
      break;
    case SCR_EDIT: case SCR_SETUP: {
      int n = edit_count();
      cursor += step + page_step; keep_visible(n);
      if (b == BTN_A) edit_toggle(cursor);
      if (screen == SCR_SETUP && (b == BTN_START || (b == BTN_B && setup_step > 0))) {
        setup_step += b == BTN_START ? 1 : -1; cursor = top = 0;
        if (setup_step > 2) { set.setup_done = 1; settings_save(&set); start_loading(0); }
        else edit_kind = (EditKind)setup_step;
      } else if (screen == SCR_EDIT && b == BTN_B) { settings_save(&set); screen = SCR_SETTINGS; cursor = edit_kind + 1; top = 0; }
      break;
    }
    case SCR_PIN:
      if (b == BTN_UP || b == BTN_DOWN) pin_digits[pin_pos] = (pin_digits[pin_pos] + (b == BTN_UP ? 1 : 9)) % 10;
      if (b == BTN_LEFT && pin_pos > 0) pin_pos--;
      if (b == BTN_RIGHT && pin_pos < 3) pin_pos++;
      if (b == BTN_A) { if (pin_pos < 3) pin_pos++; else pin_done(); }
      if (b == BTN_B) screen = pin_return;
      break;
    case SCR_SEARCH:
      if (b == BTN_LEFT) cursor = (cursor + NKEYS - 1) % NKEYS;
      if (b == BTN_RIGHT) cursor = (cursor + 1) % NKEYS;
      if (b == BTN_UP) cursor = cursor >= 10 ? cursor - 10 : cursor;
      if (b == BTN_DOWN) cursor = cursor + 10 < NKEYS ? cursor + 10 : NKEYS - 1;
      if (b == BTN_Y || b == BTN_X) { size_t n = strlen(query); if (n) query[n - 1] = 0; }   // quick delete
      if (b == BTN_A || b == BTN_START) {
        size_t n = strlen(query);
        if (b == BTN_START || cursor == NKEYS - 1) { open_list(LIST_SEARCH, 0, SCR_LIST); break; }   // OK
        if (cursor == NKEYS - 2) { if (n) query[n - 1] = 0; }                                        // DEL
        else if (n < sizeof query - 1) {
          static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
          query[n] = chars[cursor]; query[n + 1] = 0;
        }
      }
      if (b == BTN_B) { screen = SCR_INDEX; cursor = 5; top = 0; }
      break;
    case SCR_LOADING: break;
  }
}

// The old CRT switch-off: the picture squeezes to a bright line, then to a dot that fades.
static void power_off(void) {
  int w = 1024, h = 768;
  Uint32 *px = malloc(w * h * 4);
  if (!px || SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) != 0) { free(px); return; }
  SDL_Texture *last = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
  SDL_UpdateTexture(last, NULL, px, w * 4); free(px);
  SDL_PauseAudioDevice(audio, 1);
  for (int f = 0; f <= 24; f++) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    if (f <= 10) {   // squeeze vertically, brightening
      int lh = h - (h - 6) * f / 10;
      SDL_Rect d = { 0, (h - lh) / 2, w, lh };
      SDL_SetTextureColorMod(last, 255, 255, 255); SDL_RenderCopy(ren, last, NULL, &d);
      SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD); SDL_SetRenderDrawColor(ren, 255, 255, 255, (Uint8)(f * 22)); SDL_RenderFillRect(ren, &d);
    } else if (f <= 18) {   // the line shrinks to a dot
      int lw = w - (w - 8) * (f - 10) / 8;
      SDL_Rect d = { (w - lw) / 2, h / 2 - 3, lw, 6 };
      SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(ren, 255, 255, 255, 255); SDL_RenderFillRect(ren, &d);
    } else {   // the dot fades
      Uint8 v = (Uint8)(255 - (f - 18) * 40);
      SDL_Rect d = { w / 2 - 4, h / 2 - 4, 8, 8 };
      SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(ren, v, v, v, 255); SDL_RenderFillRect(ren, &d);
    }
    SDL_RenderPresent(ren);
  }
  SDL_DestroyTexture(last);
}

// --shots <dir>: draws each theme's pages to BMP files and quits (for checking the looks without pressing buttons).
static void save_shot(const char *dir, const char *name) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 1024, 768, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
  char p[700]; snprintf(p, sizeof p, "%s/%s.bmp", dir, name);
  SDL_SaveBMP(s, p); SDL_FreeSurface(s);
}
static void shots(const char *dir) {
  static const Screen scr[] = { SCR_INDEX, SCR_GENRES, SCR_LIST, SCR_GUIDE, SCR_SETTINGS, SCR_SETUP, SCR_SEARCH, SCR_PIN, SCR_WATCH };
  static const char *nm[] = { "index", "genres", "list", "guide", "settings", "setup", "search", "pin", "banner" };
  for (int t = 0; t < NTHEMES; t++) {
    set.theme = t;
    for (int k = 0; k < 9; k++) {
      screen = scr[k]; cursor = top = 0; edit_kind = EDIT_COUNTRY; setup_step = 1; pin_setting = 1; snprintf(query, sizeof query, "NRK");
      if (screen == SCR_LIST || screen == SCR_GUIDE || screen == SCR_WATCH) { list_kind = LIST_ALL; build_view(); cursor = 2; }
      for (int frame = 0; frame < (screen == SCR_LIST ? 90 : 2); frame++) {   // lists: give the logos a moment
        logos_pump();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
        if (screen == SCR_WATCH) { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3); }
        else { fill_page(); theme()->page(&page); }
        if (frame == 0 || frame == (screen == SCR_LIST ? 89 : 1)) SDL_RenderPresent(ren); else { SDL_RenderPresent(ren); SDL_Delay(30); }
      }
      // draw once more to an unpresented frame and save it
      SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
      if (screen == SCR_WATCH) { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3); } else { fill_page(); theme()->page(&page); }
      char n[64]; snprintf(n, sizeof n, "%d-%s", t, nm[k]); save_shot(dir, n);
      SDL_RenderPresent(ren);
    }
  }
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  const char *pak = argc > 2 ? argv[2] : ".";
  const char *shots_dir = argc > 4 && !strcmp(argv[3], "--shots") ? argv[4] : NULL;
  store_init(dir);
  settings_load(&set);
  snprintf(set.pak_dir, sizeof set.pak_dir, "%s", pak);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  avformat_network_init();
  av_log_set_level(AV_LOG_ERROR);
  SDL_Window *win = SDL_CreateWindow("Tuner", 0, 0, 1024, 768, SDL_WINDOW_FULLSCREEN);
  ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, 1024, 768);
  for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  SDL_AudioSpec want = {0}, have;
  want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
  audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  char font[700]; snprintf(font, sizeof font, "%s/bedstead.otf", pak);
  if (tt_init(ren, font) != 0) { fprintf(stderr, "font %s: %s\n", font, SDL_GetError()); return 1; }
  themes_init(ren);
  logos_init(ren);
  tv_setup(ren, audio, &set);
  load_lock = SDL_CreateMutex();

  if (!set.setup_done) { screen = SCR_SETUP; setup_step = 0; edit_kind = EDIT_LANG; }
  else start_loading(0);

  int held = -1; Uint32 repeat_at = 0;
  int l2 = 0, r2 = 0;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = 0;
      if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        int b = e.cbutton.button;
        on_button(b);
        if (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT) { held = b; repeat_at = SDL_GetTicks() + 400; }
      }
      if (e.type == SDL_CONTROLLERBUTTONUP && e.cbutton.button == held) held = -1;
      if (e.type == SDL_CONTROLLERAXISMOTION) {   // L2/R2: a press when pushed past half way
        int *st = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? &l2 : e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? &r2 : NULL;
        if (st) {
          int down = e.caxis.value > 16000;
          if (down && !*st) on_button(st == &l2 ? BTN_L2 : BTN_R2);
          *st = down;
        }
      }
    }
    if (held >= 0 && SDL_GetTicks() >= repeat_at && screen != SCR_WATCH) { on_button(held); repeat_at = SDL_GetTicks() + 70; }
    if (screen == SCR_LOADING && load_done) {
      channel_list_free(&chans); chans = loaded; memset(&loaded, 0, sizeof loaded);
      load_done = 0; screen = SCR_INDEX; cursor = top = 0;
      fprintf(stderr, "%d channels loaded\n", chans.n);
      if (shots_dir) { shots(shots_dir); running = 0; break; }
      // Start on the channel watched last, like a TV.
      if (set.last_channel[0]) {
        list_kind = LIST_ALL; build_view();
        for (int i = 0; i < nview; i++) if (!strcmp(view[i]->key, set.last_channel)) { screen = SCR_LIST; cursor = i; keep_visible(nview); watch(i); break; }
      }
    }
    // Live themes: resting on a channel in a list for a moment tunes it in behind the menu, like a TV's preview.
    if (theme()->live_menus && screen == SCR_LIST && nview && last_move && SDL_GetTicks() - last_move > 1200) {
      if (tv_channel() != view[cursor]) tv_tune(view[cursor]);
      last_move = 0;
    }

    if (sleep_at) {   // sleep timer
      Uint32 now = SDL_GetTicks();
      if (now >= sleep_at) running = 0;
      else if (sleep_at - now < 60000 && !sleep_warned) { sleep_warned = 1; say("SLEEP IN 1 MINUTE"); }
    }
    logos_pump();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    if (tv_state() != TV_OFF) tv_render();
    if (screen == SCR_WATCH) {
      int plain_loading = set.plain_tuning && tv_state() == TV_TUNING;
      if ((plain_loading || SDL_GetTicks() < banner_until) && nview) theme()->banner(view[watch_index], watch_index + 1);
      if (plain_loading) {   // plain tuning screen: the channel info stays up with LOADING, in the theme's style
        static const char *dots[] = { "LOADING", "LOADING.", "LOADING..", "LOADING..." };
        theme()->toast(dots[(SDL_GetTicks() / 400) % 4], 1);
      }
      if (show_osd) { char l[96]; tv_osd_line(l, sizeof l); tt_draw(12, 12, 20, l, TT_PALETTE[TT_GREEN]); }
    } else { fill_page(); theme()->page(&page); }
    if (SDL_GetTicks() < toast_until) theme()->toast(toast, 0);
    SDL_RenderPresent(ren);
  }
  if (!shots_dir) power_off();
  tv_tune(NULL);
  SDL_Quit();
  return 0;
}
