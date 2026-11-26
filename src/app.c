// Tuner – IPTV Player: free TV for NextUI. Pluto TV, Samsung TV Plus and iptv-org channels for the languages and
// countries chosen at first start, your own M3U playlists, favourites and a guide, drawn with Dot (themes.c, ui/dot).
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include "channels.h"
#include "sources.h"
#include "store.h"
#include "tv.h"
#include "ui.h"
#include "logos.h"
#include "wifi.h"
#include "dot.h"

// Pad buttons in the Brick's own names (it reports an Xbox layout with Nintendo labels).
enum { BTN_A = SDL_CONTROLLER_BUTTON_B, BTN_B = SDL_CONTROLLER_BUTTON_A, BTN_X = SDL_CONTROLLER_BUTTON_Y,
       BTN_Y = SDL_CONTROLLER_BUTTON_X, BTN_START = SDL_CONTROLLER_BUTTON_START, BTN_SELECT = SDL_CONTROLLER_BUTTON_BACK,
       BTN_MENU = SDL_CONTROLLER_BUTTON_GUIDE, BTN_L1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_R1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
       BTN_UP = SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_DOWN = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       BTN_LEFT = SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_RIGHT = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
       BTN_L2 = 100, BTN_R2 = 101 };   // the inner triggers are axes; turned into presses below

typedef enum { SCR_SETUP, SCR_LOADING, SCR_INDEX, SCR_GENRES, SCR_LIST, SCR_GUIDE, SCR_SETTINGS, SCR_EDIT, SCR_WATCH, SCR_PIN, SCR_SEARCH, SCR_MYGROUPS } Screen;
typedef enum { LIST_FAVS, LIST_GENRE, LIST_ALL, LIST_NOW, LIST_MINE, LIST_SEARCH, LIST_MOST, LIST_RECENT } ListKind;
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
static char audio_label[64]; static Uint32 audio_at;   // a notice mid-screen (only one audio track)
// LEFT/RIGHT while watching: the picker (audio | subtitles). Its tabs, which is shown, the row focused, when it was
// last used (it closes by itself after PICK_MS).
#define PICK_MS 5000
static int pick_open, pick_tab, pick_cur; static Uint32 pick_at;
typedef struct { char items[17][48]; const char *ptr[17]; int n, in_use; } PickList;
static void pick_list(int tab, PickList *l) {   // tab 0 audio, 1 subtitles ("Off" first)
  int cur;
  if (tab == 0) { l->n = tv_audio_tracks(l->items, 16, &cur); l->in_use = cur; }
  else { snprintf(l->items[0], 48, "Off"); l->n = 1 + tv_sub_tracks(l->items + 1, 16, &cur); l->in_use = cur + 1; }
  for (int i = 0; i < l->n; i++) l->ptr[i] = l->items[i];
}
static int pick_subs(void) { char l[1][48]; int c; return tv_sub_tracks(l, 1, &c) > 0; }
static int pick_audios(void) { char l[16][48]; int c; return tv_audio_tracks(l, 16, &c); }
static int prev_index = -1;                        // X while watching: back to the channel before
static char resume_key[64]; static ListKind resume_kind; static int resume_genre;   // START in the menus: back to this channel, in this list
static Uint32 sleep_at; static int sleep_min, sleep_warned;
static int pin_unlocked, pin_setting, pin_digits[4], pin_pos;   // adult PIN: entering to set (pin_setting) or to unlock
static Screen pin_return;
static char query[48];                             // search text
static char mine_group[64]; static int mine_group_all = 1, mine_back_cursor;   // your channels: the group being browsed
static char (*my_groups)[64]; static int *my_group_n, n_my_groups;   // your playlists' groups, in playlist order
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
// No Wi-Fi: the loading screen says so and waits; loading starts by itself once it's back (see main).
static int load_waiting = -1;   // -1 not waiting; else the force to load with
static void start_loading(int force) {
  load_done = 0; screen = SCR_LOADING; tv_tune(NULL);
  if (!wifi_up()) { load_waiting = force; return; }
  load_waiting = -1;
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

static int cmp_score(const void *a, const void *b) {
  double x = watch_score((*(Channel *const *)a)->key), y = watch_score((*(Channel *const *)b)->key);
  return x < y ? 1 : x > y ? -1 : 0;
}
static void build_view(void) {
  free(view); view = malloc((chans.n + 1) * sizeof *view); nview = 0;
  for (int i = 0; i < chans.n; i++) {
    Channel *c = &chans.v[i];
    if (!visible(c)) continue;
    if (list_kind == LIST_FAVS && !is_fav(c->key)) continue;
    if (list_kind == LIST_GENRE && c->genre != list_genre) continue;
    if (list_kind == LIST_NOW && !c->nprogs) continue;
    if (list_kind == LIST_MINE && (!is_mine(c) || (!mine_group_all && strcmp(c->group, mine_group)))) continue;
    if (list_kind == LIST_SEARCH && !contains_ci(c->name, query)) continue;
    if (list_kind == LIST_MOST && watch_score(c->key) < 30) continue;
    if (list_kind == LIST_RECENT) continue;   // built in recent order below
    view[nview++] = c;
  }
  if (list_kind == LIST_MOST) {   // the 20 channels watched most (time fades with a 14-day half-life)
    qsort(view, nview, sizeof *view, cmp_score);
    if (nview > 20) nview = 20;
  }
  if (list_kind == LIST_RECENT)
    for (int r = 0; r < recent_count(); r++)
      for (int i = 0; i < chans.n; i++) if (!strcmp(chans.v[i].key, recent_key(r)) && visible(&chans.v[i])) { view[nview++] = &chans.v[i]; break; }
}
static int cmp_order(const void *a, const void *b) { return (*(Channel *const *)a)->order - (*(Channel *const *)b)->order; }
static int cmp_name(const void *a, const void *b) { return strcasecmp((*(Channel *const *)a)->name, (*(Channel *const *)b)->name); }
static void sort_mine(void) { if (list_kind == LIST_MINE) qsort(view, nview, sizeof *view, set.mine_az ? cmp_name : cmp_order); }
// Your playlists' groups (group-title), in the order they first appear, with channel counts.
static void collect_my_groups(void) {
  free(my_groups); free(my_group_n); my_groups = NULL; my_group_n = NULL; n_my_groups = 0;
  Channel **mine = malloc((chans.n + 1) * sizeof *mine); int n = 0;
  for (int i = 0; i < chans.n; i++) if (is_mine(&chans.v[i]) && visible(&chans.v[i])) mine[n++] = &chans.v[i];
  qsort(mine, n, sizeof *mine, cmp_order);
  for (int i = 0; i < n; i++) {
    int g = 0; while (g < n_my_groups && strcmp(my_groups[g], mine[i]->group)) g++;
    if (g == n_my_groups) {
      my_groups = realloc(my_groups, (n_my_groups + 1) * sizeof *my_groups); my_group_n = realloc(my_group_n, (n_my_groups + 1) * sizeof *my_group_n);
      snprintf(my_groups[g], sizeof my_groups[g], "%s", mine[i]->group); my_group_n[g] = 0; n_my_groups++;
    }
    my_group_n[g]++;
  }
  free(mine);
}
static void open_list(ListKind k, int genre, Screen scr) { list_kind = k; list_genre = genre; build_view(); sort_mine(); cursor = top = 0; screen = scr; }
static void keep_visible(int n) {
  int rows = theme()->list_rows;
  if (cursor >= n) cursor = n - 1;
  if (cursor < 0) cursor = 0;
  if (cursor < top) top = cursor;
  if (cursor >= top + rows) top = cursor - rows + 1;
}
// NextUI's settings live in shared memory, read through its libmsettings: the volume, shown when it changes (the
// volume buttons themselves are NextUI's). Not on the Brick (or not under NextUI): nothing.
static int (*ms_volume)(void), (*ms_mute)(void);
static int vol_last = -1, mute_last = -1; static Uint32 vol_until, vol_checked;
static void volume_init(void) {
  void *h = dlopen("libmsettings.so", RTLD_NOW);
  if (!h) {
    char p[600]; const char *sys = getenv("SYSTEM_PATH");
    snprintf(p, sizeof p, "%s/lib/libmsettings.so", sys ? sys : "/mnt/SDCARD/.system/tg5040"); h = dlopen(p, RTLD_NOW);
  }
  if (!h || access("/dev/shm/SharedSettings", F_OK) != 0) return;   // only as a reader of NextUI's own settings
  void (*init)(void) = (void (*)(void))dlsym(h, "InitSettings");
  ms_volume = (int (*)(void))dlsym(h, "GetVolume"); ms_mute = (int (*)(void))dlsym(h, "GetMute");
  if (!init || !ms_volume) { ms_volume = NULL; return; }
  init();
}
static void volume_poll(void) {
  if (!ms_volume || SDL_GetTicks() - vol_checked < 100) return;
  vol_checked = SDL_GetTicks();
  int v = ms_volume(), m = ms_mute ? ms_mute() : 0;
  if (vol_last >= 0 && (v != vol_last || m != mute_last)) vol_until = SDL_GetTicks() + 1500;
  vol_last = v; mute_last = m;
}
static void say(const char *m) { snprintf(toast, sizeof toast, "%s", m); toast_until = SDL_GetTicks() + 1500; }

// --- menus -----------------------------------------------------------------------------------------------------------
typedef struct { const char *label; int page; } MenuItem;
static const MenuItem INDEX_ITEMS[] = { {"Favourites", 101}, {"Most watched", 102}, {"Recently watched", 103},
                                        {"Channels by genre", 200}, {"All channels", 250}, {"My channels", 260},
                                        {"Guide", 300}, {"Search", 400}, {"Settings", 900} };
#define NINDEX 9
static int index_of(int page) { for (int i = 0; i < NINDEX; i++) if (INDEX_ITEMS[i].page == page) return i; return 0; }
#define NSETTINGS 18
static void setting_row(int i, Row *r) {
  memset(r, 0, sizeof *r);
  switch (i) {
    case 0: snprintf(r->text, sizeof r->text, "Languages"); snprintf(r->detail, sizeof r->detail, "%s", set.languages); break;
    case 1: snprintf(r->text, sizeof r->text, "Countries"); snprintf(r->detail, sizeof r->detail, "%s", set.countries[0] ? set.countries : "-"); break;
    case 2: snprintf(r->text, sizeof r->text, "Genres"); break;
    case 3: snprintf(r->text, sizeof r->text, "Adult channels"); snprintf(r->detail, sizeof r->detail, "%s", set.adult ? "ON" : "OFF"); break;
    case 4: snprintf(r->text, sizeof r->text, "Max quality"); snprintf(r->detail, sizeof r->detail, "%dp", set.max_height); break;
    case 5: snprintf(r->text, sizeof r->text, "Picture"); snprintf(r->detail, sizeof r->detail, "%s  (L1/R1)", tv_overlay_name()); break;
    case 6: snprintf(r->text, sizeof r->text, "Refresh channels now"); break;
    case 7: snprintf(r->text, sizeof r->text, "Show hidden channels again"); break;
    case 8: snprintf(r->text, sizeof r->text, "Sleep timer"); if (sleep_at) snprintf(r->detail, sizeof r->detail, "%d min (%d left)", sleep_min, (int)((sleep_at - SDL_GetTicks()) / 60000) + 1); else snprintf(r->detail, sizeof r->detail, "OFF"); break;
    case 9: snprintf(r->text, sizeof r->text, "Adult PIN"); snprintf(r->detail, sizeof r->detail, "%s", set.pin[0] ? "SET (A: change, X: remove)" : "none"); break;
    case 10: snprintf(r->text, sizeof r->text, "Tuning screen"); snprintf(r->detail, sizeof r->detail, "%s", set.plain_tuning ? "Plain" : "Snow + picture"); break;
    case 11: snprintf(r->text, sizeof r->text, "My channels order"); snprintf(r->detail, sizeof r->detail, "%s", set.mine_az ? "A-Z" : "Playlist"); break;
    case 12: snprintf(r->text, sizeof r->text, "Free channels"); snprintf(r->detail, sizeof r->detail, "%s", set.free_off ? "OFF (only your playlists)" : "ON"); break;
    case 13: snprintf(r->text, sizeof r->text, "Your playlists"); snprintf(r->detail, sizeof r->detail, "%s", set.mine_off ? "OFF (only free channels)" : "ON"); break;
    case 14: snprintf(r->text, sizeof r->text, "Suggest favourites"); snprintf(r->detail, sizeof r->detail, "%s", set.suggest_favs ? "ON" : "OFF"); break;
    case 15: snprintf(r->text, sizeof r->text, "Hardware decoding"); snprintf(r->detail, sizeof r->detail, "%s", set.hwdec ? "ON" : "OFF"); break;
    case 16: snprintf(r->text, sizeof r->text, "Crop black borders"); snprintf(r->detail, sizeof r->detail, "%s  (L2/R2)", set.autocrop ? "AUTO" : "OFF"); break;
    case 17: snprintf(r->text, sizeof r->text, "Run first-time setup again"); break;
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
#define LEAVE_MS 250
static Uint32 leave_at;   // loading done: when the card started going out
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
        int pg = INDEX_ITEMS[i].page;
        int n = pg == 101 ? count_where(f_fav, 0) : pg == 250 ? count_where(f_all, 0) : pg == 260 ? count_where(f_mine, 0) : -1;
        if (n >= 0) snprintf(r->detail, sizeof r->detail, "%d", n);
      }
      p->hints[0] = "A Open"; p->hints[1] = resume_key[0] ? "B TV" : "MENU Quit"; p->hints[2] = resume_key[0] ? "MENU Quit" : "";
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
      static const char *names[] = { "Favourites", "", "All channels", "Guide", "My channels", "Search", "Most watched", "Recently watched" };
      static const int pages[] = { 101, 0, 250, 300, 260, 401, 102, 103 };
      p->kind = screen == SCR_GUIDE ? PAGE_GUIDE : PAGE_LIST;
      p->page_no = list_kind == LIST_GENRE ? 201 + list_genre : pages[list_kind];
      if (list_kind == LIST_SEARCH) snprintf(p->title, sizeof p->title, "Search: %s", query);
      else if (list_kind == LIST_MINE && !mine_group_all) snprintf(p->title, sizeof p->title, "%s", mine_group[0] ? mine_group : "(no group)");
      else snprintf(p->title, sizeof p->title, "%s", list_kind == LIST_GENRE ? GENRE_NAMES[list_genre] : names[list_kind]);
      p->chs = view; p->nchs = nview;
      for (int i = 0; i < nview && i < 4096; i++) {
        Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
        r->number = i + 1; snprintf(r->text, sizeof r->text, "%s", view[i]->name); r->fav = is_fav(view[i]->key);
        const Programme *cur, *next; now_next(view[i], now, &cur, &next);
        if (cur) snprintf(r->detail, sizeof r->detail, "%s", cur->title);
        if (list_kind == LIST_MOST) { int m = (int)(watch_score(view[i]->key) / 60); snprintf(r->detail, sizeof r->detail, "%dh %02dm", m / 60, m % 60); }
        if (screen == SCR_LIST && i >= top - theme()->list_rows && i <= top + 2 * theme()->list_rows) r->logo = logo_get(view[i]->logo);
      }
      if (!nview) snprintf(p->help, sizeof p->help, "%s", list_kind == LIST_FAVS ? "No favourites yet: press Y on a channel." :
                           list_kind == LIST_MOST || list_kind == LIST_RECENT ? "Channels you watch show up here by themselves." :
                           list_kind == LIST_MINE ? (sources_warning[0] ? sources_warning : "Add playlists to playlists.txt in Tuner.pak") : "Nothing here.");
      snprintf(p->status, sizeof p->status, "%d/%d", nview ? cursor + 1 : 0, nview);
      p->hints[0] = "A Watch"; p->hints[1] = "B Back"; p->hints[2] = "Y Fav"; p->hints[3] = screen == SCR_GUIDE ? "" : "X Hide";
      break;
    }
    case SCR_SETTINGS:
      p->kind = PAGE_MENU; p->page_no = 900; snprintf(p->title, sizeof p->title, "Settings");
      for (int i = 0; i < NSETTINGS; i++) setting_row(i, &p->rows[p->nrows++]);
      p->hints[0] = "A Change"; p->hints[1] = "B Back";
      break;
    case SCR_SETUP:
      if (setup_step == 0) {   // first question: free channels too, or only your own playlists?
        p->kind = PAGE_MENU; p->page_no = 800; snprintf(p->title, sizeof p->title, "Setup: Which channels?");
        snprintf(p->help, sizeof p->help, "Your own playlists go in playlists.txt in Tuner.pak");
        Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r); snprintf(r->text, sizeof r->text, "Free channels + your playlists");
        snprintf(r->detail, sizeof r->detail, "Pluto, Samsung, iptv-org");
        r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r); snprintf(r->text, sizeof r->text, "Only your own playlists");
        snprintf(r->detail, sizeof r->detail, "playlists.txt");
        p->hints[0] = "A Choose";
        break;
      }
      /* fall through */
    case SCR_EDIT: {
      static const char *titles[] = { "Languages", "Countries", "Genres" };
      static const char *help[] = { "Channels in these languages (iptv-org).", "Free TV services and national channels.", "Genres to show. Adult is off unless set." };
      p->kind = PAGE_CHECK;
      p->page_no = screen == SCR_SETUP ? 800 + setup_step : 910 + edit_kind;
      if (screen == SCR_SETUP) snprintf(p->title, sizeof p->title, "Setup %d/4: %s", setup_step + 1, titles[edit_kind]);
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
      p->kind = PAGE_LOADING; p->page_no = 199;
      if (leave_at) p->leave = (SDL_GetTicks() - leave_at) / (float)LEAVE_MS;
      if (load_waiting >= 0) {
        snprintf(p->title, sizeof p->title, "No Wi-Fi");
        snprintf(p->status, sizeof p->status, "Tuner needs Wi-Fi. Turn it on in the device settings: Tuner goes on by itself.");
        break;
      }
      snprintf(p->title, sizeof p->title, "Tuning in");
      SDL_LockMutex(load_lock); snprintf(p->status, sizeof p->status, "%s", load_msg); SDL_UnlockMutex(load_lock);
      break;
    case SCR_MYGROUPS: {
      p->kind = PAGE_MENU; p->page_no = 260; snprintf(p->title, sizeof p->title, "My channels");
      int total = 0; for (int g = 0; g < n_my_groups; g++) total += my_group_n[g];
      Row *r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
      snprintf(r->text, sizeof r->text, "All my channels"); snprintf(r->detail, sizeof r->detail, "%d", total);
      for (int g = 0; g < n_my_groups && p->nrows < 4096; g++) {
        r = &p->rows[p->nrows++]; memset(r, 0, sizeof *r);
        snprintf(r->text, sizeof r->text, "%s", my_groups[g][0] ? my_groups[g] : "(no group)"); snprintf(r->detail, sizeof r->detail, "%d", my_group_n[g]);
      }
      p->hints[0] = "A Open"; p->hints[1] = "B Back";
      break;
    }
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
// Watching time for Most watched / Recently watched: counted while the picture plays; a viewing counts from 30 s.
static Channel *session_ch; static Uint32 session_ms;
static void watch_commit(int final) {
  if (!session_ch || (final && session_ms < 30000) || (!final && session_ms < 300000)) { if (final) { session_ch = NULL; session_ms = 0; } return; }
  watch_add(session_ch->key, session_ms / 1000.0);
  if (set.suggest_favs && !is_fav(session_ch->key) && !was_suggested(session_ch->key) && watch_score(session_ch->key) > 7200) {
    mark_suggested(session_ch->key); snprintf(toast, sizeof toast, "Watch this a lot? Y makes it a favourite"); toast_until = SDL_GetTicks() + 6000;
  }
  session_ms = 0;
  if (final) session_ch = NULL;
}

static void no_wifi(void) { snprintf(toast, sizeof toast, "No Wi-Fi: turn it on to watch"); toast_until = SDL_GetTicks() + 3000; }
static void tune_view(int i) {
  if (!nview) return;
  if (!wifi_up()) { no_wifi(); return; }
  int next = (i + nview) % nview;
  if (screen == SCR_WATCH && next != watch_index) prev_index = watch_index;
  watch_index = next;
  watch_commit(1);
  if (strcmp(set.last_channel, view[watch_index]->key)) { snprintf(set.last_channel, sizeof set.last_channel, "%s", view[watch_index]->key); settings_save(&set); }
  tv_tune(view[watch_index]);
  banner_until = SDL_GetTicks() + 6000;
}
static void watch(int i) {
  if (!wifi_up()) { no_wifi(); return; }
  back_from_watch = screen; prev_index = -1;
  snprintf(resume_key, sizeof resume_key, "%s", view[i]->key); resume_kind = list_kind; resume_genre = list_genre;
  if (tv_channel() == view[i] && tv_state() != TV_OFF) {   // still playing behind the menu: just back to it
    watch_index = i; screen = SCR_WATCH; banner_until = SDL_GetTicks() + 6000; return;
  }
  tune_view(i); screen = SCR_WATCH;
}
// START in a menu: back to the channel you were watching (found again in its list; re-tuned if it was stopped).
static int resume_watch(void) {
  if (!resume_key[0]) return 0;
  ListKind lk = list_kind; int lg = list_genre;
  list_kind = resume_kind; list_genre = resume_genre; build_view(); sort_mine();
  for (int i = 0; i < nview; i++) if (!strcmp(view[i]->key, resume_key)) {
    cursor = i; keep_visible(nview); screen = SCR_LIST; watch(i); return 1;
  }
  list_kind = lk; list_genre = lg; build_view(); sort_mine();
  say("That channel is gone");
  return 0;
}
static void leave_watch(void) {
  watch_commit(1);
  screen = back_from_watch;
  if (!theme()->live_menus) tv_tune(NULL);   // themes that cover the screen stop the channel in the menus
  build_view(); sort_mine();
  cursor = watch_index < nview ? watch_index : nview - 1; keep_visible(nview);
}

static void toggle_crop(void) { set.autocrop = !set.autocrop; settings_save(&set); say(set.autocrop ? "Crop black borders: auto" : "Crop black borders: off"); }

static void ask_pin(int setting, Screen ret) { pin_setting = setting; pin_return = ret; pin_pos = 0; memset(pin_digits, 0, sizeof pin_digits); screen = SCR_PIN; }
static void pin_done(void) {
  char entered[5]; for (int i = 0; i < 4; i++) entered[i] = (char)('0' + pin_digits[i]); entered[4] = 0;
  if (pin_setting) { snprintf(set.pin, sizeof set.pin, "%s", entered); settings_save(&set); pin_unlocked = 0; say("PIN set"); screen = SCR_SETTINGS; return; }
  if (!strcmp(entered, set.pin)) { pin_unlocked = 1; say("Unlocked"); open_list(LIST_GENRE, G_ADULT, SCR_LIST); }
  else { say("Wrong PIN"); screen = pin_return; }
}

static void on_button(int b) {
  if (b == BTN_MENU) { running = 0; return; }
  if (b == BTN_START && screen != SCR_WATCH && screen != SCR_SETUP && screen != SCR_EDIT && screen != SCR_PIN && screen != SCR_SEARCH
      && screen != SCR_LOADING && resume_watch()) return;
  int rows = theme()->list_rows;
  int page_step = (b == BTN_L1 || b == BTN_LEFT) ? -rows : (b == BTN_R1 || b == BTN_RIGHT) ? rows : 0;
  int step = b == BTN_UP ? -1 : b == BTN_DOWN ? 1 : 0;
  switch (screen) {
    case SCR_WATCH:
      if (pick_open) {   // the picker has the buttons: UP/DOWN pick at once, LEFT/RIGHT the other tab, A/B close
        PickList l; pick_list(pick_tab, &l);
        pick_at = SDL_GetTicks();
        if ((b == BTN_UP || b == BTN_DOWN) && l.n) {
          pick_cur = (pick_cur + (b == BTN_DOWN ? 1 : -1) + l.n) % l.n;
          if (pick_tab == 0) tv_audio_set(pick_cur); else tv_sub_set(pick_cur - 1);
        } else if ((b == BTN_LEFT || b == BTN_RIGHT) && pick_audios() >= 2 && pick_subs()) {
          pick_tab = !pick_tab; pick_list(pick_tab, &l); pick_cur = l.in_use >= 0 ? l.in_use : 0;
        } else if (b == BTN_A || b == BTN_B || b == BTN_SELECT || b == BTN_START) pick_open = 0;
        break;
      }
      if (b == BTN_UP || b == BTN_DOWN) tune_view(watch_index + (b == BTN_UP ? 1 : -1));
      if (b == BTN_A) banner_until = SDL_GetTicks() < banner_until ? 0 : 0xFFFFFFFFu;
      if (b == BTN_Y) { toggle_fav(view[watch_index]->key); say(is_fav(view[watch_index]->key) ? "Favourite" : "Not a favourite"); }
      if (b == BTN_L1 || b == BTN_R1) { tv_overlay_step(b == BTN_R1 ? 1 : -1); say(tv_overlay_name()); }
      if (b == BTN_L2 || b == BTN_R2) toggle_crop();
      if (b == BTN_SELECT) show_osd = !show_osd;
      if ((b == BTN_LEFT || b == BTN_RIGHT) && !pick_open) {   // the picker: audio, and subtitles when there are some
        int na = pick_audios(), ns = pick_subs();
        if (na < 2 && !ns) { snprintf(audio_label, sizeof audio_label, na ? "ONE AUDIO TRACK" : "NO AUDIO"); audio_at = SDL_GetTicks(); }
        else {
          pick_open = 1; pick_tab = na < 2 ? 1 : 0; pick_at = SDL_GetTicks();
          PickList l; pick_list(pick_tab, &l); pick_cur = l.in_use >= 0 ? l.in_use : 0;
        }
      }
      if (b == BTN_X && prev_index >= 0 && prev_index < nview) tune_view(prev_index);   // last channel
      if (b == BTN_B) leave_watch();
      break;
    case SCR_INDEX:
      if (step) cursor = (cursor + step + NINDEX) % NINDEX;
      if (b == BTN_B) { resume_watch(); break; }   // the front page sits on top of the TV: B goes back to it
      if (b == BTN_A) {
        int pg = INDEX_ITEMS[cursor].page; cursor = top = 0;
        if (pg == 101) open_list(LIST_FAVS, 0, SCR_LIST);
        else if (pg == 102) open_list(LIST_MOST, 0, SCR_LIST);
        else if (pg == 103) open_list(LIST_RECENT, 0, SCR_LIST);
        else if (pg == 200) screen = SCR_GENRES;
        else if (pg == 250) open_list(LIST_ALL, 0, SCR_LIST);
        else if (pg == 260) {
          collect_my_groups();
          if (n_my_groups > 1) { screen = SCR_MYGROUPS; cursor = top = 0; }
          else { mine_group_all = 1; open_list(LIST_MINE, 0, SCR_LIST); }
        }
        else if (pg == 300) open_list(LIST_NOW, 0, SCR_GUIDE);
        else if (pg == 400) { screen = SCR_SEARCH; cursor = 0; }
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
      if (b == BTN_B) { screen = SCR_INDEX; cursor = index_of(200); top = 0; }
      break;
    }
    case SCR_LIST: case SCR_GUIDE:
      cursor += step + page_step; keep_visible(nview);
      if (step || page_step) last_move = SDL_GetTicks();
      if (b == BTN_A) watch(cursor);
      if (b == BTN_Y && nview) { toggle_fav(view[cursor]->key); if (list_kind == LIST_FAVS) { build_view(); keep_visible(nview); } }
      if (b == BTN_X && nview && screen == SCR_LIST) { toggle_hidden(view[cursor]->key); say("Hidden"); build_view(); sort_mine(); keep_visible(nview); }
      if (b == BTN_B) {
        if (list_kind == LIST_GENRE) { screen = SCR_GENRES; cursor = top = 0; }
        else if (list_kind == LIST_SEARCH) { screen = SCR_SEARCH; cursor = NKEYS - 1; top = 0; }
        else if (list_kind == LIST_MINE && n_my_groups > 1) { screen = SCR_MYGROUPS; cursor = mine_back_cursor; top = 0; keep_visible(n_my_groups + 1); }
        else {
          static const int back_page[] = { 101, 200, 250, 300, 260, 400, 102, 103 };
          screen = SCR_INDEX; cursor = index_of(back_page[list_kind]); top = 0;
        }
      }
      break;
    case SCR_SETTINGS:
      if (step) { cursor = (cursor + step + NSETTINGS) % NSETTINGS; keep_visible(NSETTINGS); }
      if (b == BTN_A || ((b == BTN_LEFT || b == BTN_RIGHT) && (cursor == 5 || cursor == 10 || cursor == 11 || cursor == 12 || cursor == 13 || cursor == 14 || cursor == 15 || cursor == 16))) {
        int dir = b == BTN_LEFT ? -1 : 1;
        if (cursor <= 2) { edit_kind = (EditKind)cursor; screen = SCR_EDIT; cursor = top = 0; }
        else if (cursor == 3) { set.adult = !set.adult; dirty_sources = 1; settings_save(&set); }
        else if (cursor == 4) { set.max_height = set.max_height == 480 ? 720 : set.max_height == 720 ? 1080 : 480; dirty_sources = 1; settings_save(&set); }
        else if (cursor == 5) tv_overlay_step(dir);
        else if (cursor == 6) { dirty_sources = 0; start_loading(1); }
        else if (cursor == 7) { unhide_all(); say("All channels shown"); }
        else if (cursor == 8) {
          static const int steps[] = { 0, 30, 60, 90, 120 };
          int k = 0; while (k < 4 && steps[k] != sleep_min) k++;
          sleep_min = steps[(k + 1) % 5]; sleep_at = sleep_min ? SDL_GetTicks() + sleep_min * 60000u : 0; sleep_warned = 0;
        }
        else if (cursor == 9) ask_pin(1, SCR_SETTINGS);
        else if (cursor == 10) { set.plain_tuning = !set.plain_tuning; settings_save(&set); }
        else if (cursor == 11) { set.mine_az = !set.mine_az; settings_save(&set); }
        else if (cursor == 12) {   // never both off: that would be no channels at all
          set.free_off = !set.free_off; if (set.free_off && set.mine_off) { set.mine_off = 0; say("Your playlists back on"); }
          settings_save(&set); dirty_sources = 1;
        }
        else if (cursor == 13) {
          set.mine_off = !set.mine_off; if (set.mine_off && set.free_off) { set.free_off = 0; say("Free channels back on"); }
          settings_save(&set); dirty_sources = 1;
        }
        else if (cursor == 14) { set.suggest_favs = !set.suggest_favs; settings_save(&set); }
        else if (cursor == 15) { set.hwdec = !set.hwdec; tv_hwdec(set.hwdec); settings_save(&set); say(set.hwdec ? "Hardware decoding on" : "Hardware decoding off"); }
        else if (cursor == 16) toggle_crop();
        else if (cursor == 17) { screen = SCR_SETUP; setup_step = 0; edit_kind = EDIT_LANG; cursor = top = 0; }
      }
      if (b == BTN_X && cursor == 9 && set.pin[0]) { set.pin[0] = 0; settings_save(&set); say("PIN removed"); }
      if (b == BTN_B) { screen = SCR_INDEX; cursor = index_of(900); top = 0; if (dirty_sources) { dirty_sources = 0; start_loading(0); } }
      break;
    case SCR_SETUP:
      if (setup_step == 0) {
        if (step) cursor = (cursor + step + 2) % 2;
        if (b == BTN_A) {
          set.free_off = cursor == 1; set.mine_off = 0;   // setup brings your playlists back either way
          if (set.free_off) { set.setup_done = 1; settings_save(&set); start_loading(0); }   // only your playlists: done
          else { setup_step = 1; edit_kind = EDIT_LANG; cursor = top = 0; }
        }
        break;
      }
      /* fall through */
    case SCR_EDIT: {
      int n = edit_count();
      cursor += step + page_step; keep_visible(n);
      if (b == BTN_A) edit_toggle(cursor);
      if (screen == SCR_SETUP && (b == BTN_START || (b == BTN_B && setup_step > 0))) {
        setup_step += b == BTN_START ? 1 : -1; cursor = top = 0;
        if (setup_step > 3) { set.setup_done = 1; settings_save(&set); start_loading(0); }
        else if (setup_step >= 1) edit_kind = (EditKind)(setup_step - 1);
        else cursor = set.free_off;   // back to the first question
      } else if (screen == SCR_EDIT && b == BTN_B) { settings_save(&set); screen = SCR_SETTINGS; cursor = edit_kind; top = 0; }
      break;
    }
    case SCR_MYGROUPS:
      cursor += step + page_step; keep_visible(n_my_groups + 1);
      if (b == BTN_A) {
        mine_group_all = cursor == 0;
        if (cursor > 0) snprintf(mine_group, sizeof mine_group, "%s", my_groups[cursor - 1]);
        int back = cursor; open_list(LIST_MINE, 0, SCR_LIST); mine_back_cursor = back;
      }
      if (b == BTN_B) { screen = SCR_INDEX; cursor = index_of(260); top = 0; }
      break;
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
      if (b == BTN_B) { screen = SCR_INDEX; cursor = index_of(400); top = 0; }
      break;
    case SCR_LOADING: break;
  }
}

// The old CRT switch-off: the picture squeezes to a bright line, then to a dot that fades.
static void power_off(void) {
  int w = DOT_W, h = DOT_H;
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

// --shots <dir>: draws each page to BMP files and quits (for checking the look without pressing buttons).
static void save_shot(const char *dir, const char *name) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, DOT_W, DOT_H, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
  char p[700]; snprintf(p, sizeof p, "%s/%s.bmp", dir, name);
  SDL_SaveBMP(s, p); SDL_FreeSurface(s);
}
static void shots(const char *dir) {
  themes_still(1);
  static const Screen scr[] = { SCR_INDEX, SCR_GENRES, SCR_LIST, SCR_GUIDE, SCR_SETTINGS, SCR_SETUP, SCR_SEARCH, SCR_PIN, SCR_WATCH, SCR_MYGROUPS, SCR_LIST };
  static const char *nm[] = { "index", "genres", "list", "guide", "settings", "setup", "search", "pin", "banner", "mygroups", "list-scrolled" };
  for (int t = 0; t < NTHEMES; t++) {
    set.theme = t;
    collect_my_groups();
    for (int k = 0; k < 11; k++) {
      screen = scr[k]; cursor = top = 0; edit_kind = EDIT_COUNTRY; setup_step = 2; pin_setting = 1; snprintf(query, sizeof query, "NRK");
      if (screen == SCR_LIST || screen == SCR_GUIDE || screen == SCR_WATCH) { list_kind = LIST_ALL; build_view(); cursor = k == 10 ? 12 : 2; }   // list-scrolled: the row before peeks out
      if (screen == SCR_GUIDE || screen == SCR_WATCH)   // a made-up guide for the first channels that have none
        for (int i = 0; i < nview && i < 8; i++) {
          const Programme *cur, *nx; now_next(view[i], time(NULL), &cur, &nx);
          if (cur) continue;
          static const char *ti[] = { "Evening News", "The Weather", "Live: Markets at the Close with a Long Title Indeed", "Documentary" };
          time_t h0 = time(NULL) / 1800 * 1800 - 900 * (i % 2);
          view[i]->progs = calloc(2, sizeof(Programme)); view[i]->nprogs = 2;
          for (int j = 0; j < 2; j++) {
            view[i]->progs[j].start = h0 + j * 1800; view[i]->progs[j].stop = h0 + (j + 1) * 1800;
            snprintf(view[i]->progs[j].title, sizeof view[i]->progs[j].title, "%s", ti[(i + j * 2) % 4]);
          }
        }
      if (screen == SCR_GUIDE && nview > 2) { if (!is_fav(view[1]->key)) toggle_fav(view[1]->key); if (!is_fav(view[2]->key)) toggle_fav(view[2]->key); }
      for (int frame = 0; frame < (screen == SCR_LIST ? 90 : 40); frame++) {   // lists: logos; all: a page's arrival
        logos_pump();
        ui_frame(); SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
        if (screen == SCR_WATCH) { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3, 1); }
        else { fill_page(); theme()->page(&page); }
        SDL_RenderPresent(ren); SDL_Delay(30);
      }
      // draw once more to an unpresented frame and save it
      ui_frame(); SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
      if (screen == SCR_WATCH) { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3, 1); } else { fill_page(); theme()->page(&page); }
      save_shot(dir, nm[k]); (void)t;
      SDL_RenderPresent(ren);
    }
  }
  // The screens the loop above can't reach: loading, waiting for Wi-Fi, NO SIGNAL and stats for nerds.
  for (int k = 0; k < 8; k++) {
    ui_frame(); SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    if (k < 2) {
      screen = SCR_LOADING; load_waiting = k ? 0 : -1;
      snprintf(load_msg, sizeof load_msg, "Samsung TV Plus: 412 channels");
      fill_page(); theme()->page(&page); load_waiting = -1;
    } else if (k == 2) ui_no_signal("NRK1", "No picture after 15 seconds. Tuner tried 3 times; it will try again next time.");
    else {
      char l[12][96] = { "CHANNEL|NRK1 (iptv-org)", "VIDEO|h264 1280x720  25.00 fps  HARDWARE", "CROP|none needed",
        "FRAMES|decoded 25.0/s  shown 25.0/s  dropped 0", "SYNC|A/V +12 ms  jumps 0  render 60/s",
        "QUEUES|frames 4/8  packets 31  audio 180 ms", "AUDIO|aac 48000 Hz 2 ch  track 1 of 2", "TRACKS|1 Norwegian  2 English",
        "STREAM|2.4 Mbps now  variant 3 of 4 (2.5 Mbps)", "HEALTH|retries 0  damaged frames 0", "DEVICE|CPU 38%  51 C  Wi-Fi -54 dBm, 72 Mbit/s" };
      if (k == 3) { draw_testcard(); ui_nerd(l, 11); }
      else if (k == 4) { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3, 1); ui_audio("AUDIO 2/3: English", SDL_GetTicks()); }
      else if (k == 5) { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3, 1); ui_busy(1, "TUNING IN", nview ? view[2 < nview ? 2 : 0]->name : "NRK1", 1); }
      else if (k == 6) {
        static const char *tabs[2] = { "Audio", "Subtitles" }, *items[4] = { "Off", "English", "Norsk", "English · SDH" };
        draw_testcard(); ui_picker(1, tabs, 2, 1, items, 4, 2, 1);
      } else { draw_testcard(); if (nview) theme()->banner(view[2 < nview ? 2 : 0], 3, 1); ui_subtitle("I told you, the signal comes back\nwhen the weather clears up."); }
    }
    static const char *xn[] = { "loading", "nowifi", "nosignal", "nerd", "audio", "busy", "picker", "subtitle" };
    save_shot(dir, xn[k]); SDL_RenderPresent(ren);
  }
}


// ffmpeg's error messages can quote stream and playlist URLs, and yours hold your provider and often a login. Every
// URL in them is replaced by [url] before it reaches the log, so no provider, login or address is ever written.
static void log_masked(void *avcl, int level, const char *fmt, va_list vl) {
  (void)avcl;
  if (level > av_log_get_level()) return;
  char line[1024], out[1024]; vsnprintf(line, sizeof line, fmt, vl);
  size_t o = 0;
  for (const char *p = line; *p && o < sizeof out - 8; ) {
    if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8) || !strncmp(p, "rtmp://", 7) || !strncmp(p, "rtsp://", 7)) {
      memcpy(out + o, "[url]", 5); o += 5;
      while (*p && *p != ' ' && *p != '\'' && *p != '"' && *p != '\n') p++;
    } else out[o++] = *p++;
  }
  out[o] = 0;
  fputs(out, stderr);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  const char *pak = argc > 2 ? argv[2] : ".";
  const char *shots_dir = argc > 4 && !strcmp(argv[3], "--shots") ? argv[4] : NULL;
  store_init(dir);
  settings_load(&set);
  if (set.theme < 0 || set.theme >= NTHEMES) set.theme = 0;   // a look that has since been removed
  snprintf(set.pak_dir, sizeof set.pak_dir, "%s", pak);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  avformat_network_init();
  av_log_set_level(AV_LOG_ERROR);
  av_log_set_callback(log_masked);
  dot_screen_detect();   // Brick 1024x768 or Smart Pro 1280x720
  SDL_Window *win = SDL_CreateWindow("Tuner", 0, 0, DOT_W, DOT_H, SDL_WINDOW_FULLSCREEN);
  ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);   // screenshots: SDL's dummy video has only the software one
  SDL_RenderSetLogicalSize(ren, DOT_W, DOT_H);
  for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  SDL_AudioSpec want = {0}, have;
  want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
  audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (themes_init(ren, pak) != 0) return 1;
  logos_init(ren);
  tv_setup(ren, audio, &set);
  tv_hwdec(set.hwdec);
  volume_init();
  load_lock = SDL_CreateMutex();
  if (!shots_dir) wifi_start();

  if (!set.setup_done) { screen = SCR_SETUP; setup_step = 0; edit_kind = EDIT_LANG; cursor = 0; }
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
    // Loaded: the card goes out (fading, upwards) for LEAVE_MS first, then the menu.
    if (screen == SCR_LOADING && load_done && !shots_dir && !leave_at) leave_at = SDL_GetTicks();
    if (screen == SCR_LOADING && load_done && (shots_dir || SDL_GetTicks() - leave_at >= LEAVE_MS)) {
      leave_at = 0;
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
    if (screen == SCR_LOADING && load_waiting >= 0 && wifi_up()) start_loading(load_waiting);
    if (theme()->live_menus && screen == SCR_LIST && nview && last_move && wifi_up() && SDL_GetTicks() - last_move > 1200) {
      if (tv_channel() != view[cursor]) tv_tune(view[cursor]);
      last_move = 0;
    }

    if (sleep_at) {   // sleep timer
      Uint32 now = SDL_GetTicks();
      if (now >= sleep_at) running = 0;
      else if (sleep_at - now < 60000 && !sleep_warned) { sleep_warned = 1; say("Sleep in 1 minute"); }
    }
    logos_pump();
    if (guide_arrived) { guide_arrived = 0; say("TV guide updated (used next start)"); }
    ui_frame();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    if (tv_state() != TV_OFF) tv_render();
    static Uint32 last_frame; Uint32 now_f = SDL_GetTicks();
    if (screen == SCR_WATCH && tv_state() == TV_PLAYING && nview) {
      if (session_ch != view[watch_index]) { session_ch = view[watch_index]; session_ms = 0; }
      if (last_frame && now_f - last_frame < 1000) session_ms += now_f - last_frame;
      watch_commit(0);   // long sessions are saved every 5 minutes
    }
    last_frame = now_f;
    if (screen == SCR_WATCH) {
      int plain_loading = set.plain_tuning && tv_state() == TV_TUNING;
      // stats for nerds (SELECT) is shown alone: the info goes down, the messages wait
      if (nview) theme()->banner(view[watch_index], watch_index + 1, !show_osd && (plain_loading || SDL_GetTicks() < banner_until));
      // plain tuning screen: the channel info stays up with LOADING (or RECONNECTING), in the theme's style; and
      // BUFFERING over a frozen picture when the stream runs dry while playing
      {   // waiting: a card in the middle with a ring of dots running round (none without Wi-Fi: nothing to wait for)
        int r = tv_retries(), nowifi = !wifi_up();
        char l[48];
        if (nowifi) snprintf(l, sizeof l, "NO WI-FI");
        else if (!plain_loading) snprintf(l, sizeof l, "BUFFERING");
        else if (r) snprintf(l, sizeof l, "RECONNECTING %d OF 3", r);
        else snprintf(l, sizeof l, "TUNING IN");
        ui_busy(!show_osd && (nowifi || plain_loading || tv_buffering()), l,
                nowifi ? "Turn it on to keep watching" : nview ? view[watch_index]->name : "", !nowifi);
      }
      if (!show_osd) { char sub[512]; tv_subtitle(sub, sizeof sub); ui_subtitle(sub); }
      if (pick_open && SDL_GetTicks() - pick_at > PICK_MS) pick_open = 0;
      {
        static const char *tabs[2] = { "Audio", "Subtitles" };
        PickList l; pick_list(pick_tab, &l);
        int two = pick_audios() >= 2 && pick_subs();
        ui_picker(pick_open && !show_osd, two ? tabs : tabs + pick_tab, two ? 2 : 1, two ? pick_tab : 0, l.ptr, l.n, pick_cur, l.in_use);
      }
      if (audio_at && !show_osd && !ui_audio(audio_label, audio_at)) audio_at = 0;
      if (show_osd) {   // stats for nerds (SELECT)
        char l[12][96]; int n = tv_nerd(l, 12);
        ui_nerd(l, n);
      }
    } else { fill_page(); theme()->page(&page); }
    int nerd_alone = screen == SCR_WATCH && show_osd;
    if (SDL_GetTicks() < toast_until && !nerd_alone) theme()->toast(toast, 0);
    volume_poll();
    if (SDL_GetTicks() < vol_until && theme()->volume && !nerd_alone) theme()->volume(vol_last, 20, mute_last);
    SDL_RenderPresent(ren);
  }
  watch_commit(1);
  if (!shots_dir) power_off();
  tv_tune(NULL);
  SDL_Quit();
  return 0;
}
