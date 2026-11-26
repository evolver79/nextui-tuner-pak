// What a screen shows, independent of how it looks: the app fills a Page, the theme draws it.
#pragma once
#include <SDL2/SDL.h>
#include "channels.h"

typedef enum { PAGE_MENU, PAGE_LIST, PAGE_CHECK, PAGE_GUIDE, PAGE_LOADING, PAGE_PIN, PAGE_KEYS } PageKind;

typedef struct {
  int number;          // channel number (lists) or page number (menus); 0 = none
  char text[160];      // name or label
  char detail[160];    // now playing, or a setting's value
  int fav;             // favourite star
  int checked;         // PAGE_CHECK: 0/1
  SDL_Texture *logo;   // channel logo, when it has arrived
} Row;

typedef struct {
  PageKind kind;
  int page_no;              // teletext page number
  char title[64];
  char help[96];            // a line under the title
  Row *rows; int nrows;
  int cursor, top;          // highlighted row, first row shown
  const char *hints[4];     // button labels for the dock: "A Watch", "B Back", ...
  char status[128];         // loading message / position
  float leave;              // PAGE_LOADING: 0, or how far the card has gone on its way out (0-1)
  Channel **chs; int nchs;  // PAGE_GUIDE
  int tv_on;                // a channel is playing behind the menu
  char input[64];           // PAGE_PIN: the digits ("12_ _"), PAGE_KEYS: the text typed so far
  const char *keys;         // PAGE_KEYS: the keyboard, rows separated by |; cursor = key index
} Page;

typedef struct {
  const char *name;
  int list_rows;        // rows a list page shows
  int live_menus;       // menus are drawn over the live channel (else the channel stops in the menus)
  void (*page)(const Page *p);
  void (*banner)(const Channel *c, int number, int up);   // over the picture while watching, every frame: up or on its way down
  void (*toast)(const char *msg, int line);       // short messages, top right
  void (*volume)(int v, int max, int muted);       // the volume, for a moment after it changes
} Theme;

extern const Theme *THEMES[]; extern const int NTHEMES;
int  themes_init(SDL_Renderer *r, const char *pak_dir);   // the Dot UI, fonts from <pak>/fonts; 0 on success
void ui_frame(void);          // once per frame, before anything is drawn
void themes_still(int on);    // screenshots: nothing blinks
void draw_testcard(void);     // the test card (also shown by the TV engine on long waits)
// The TV engine's own screens, in the same look:
void ui_searching(void);                                  // over the test card on a long wait
void ui_no_signal(const char *name, const char *reason);  // the channel couldn't be played
void ui_notice(const char *msg);                          // a line at the top, over the picture (retrying)
void ui_nerd(char lines[][96], int n);                    // stats for nerds: "LABEL|value" lines
// The audio track just picked, in the middle of the screen: "AUDIO 2/3: English" (or a line alone); since = when.
// Returns 0 once it has faded out.
int ui_audio(const char *label, Uint32 since);
// Waiting while watching (tuning in, buffering...): in the middle; drawn every frame, show says whether it's up.
void ui_busy(int show, const char *label, const char *value, int spin);
// The audio/subtitles picker, every frame while watching (show: whether it's open; it fades).
void ui_picker(int show, const char *const *tabs, int ntabs, int tab, const char *const *items, int n, int cursor, int in_use);
// The subtitle on now ("" none), above the band when that's up.
void ui_subtitle(const char *text);
