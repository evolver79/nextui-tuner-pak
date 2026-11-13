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
  const char *hints[4];     // button labels: A, B, Y, X/START
  char status[128];         // loading message / position
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
  void (*banner)(const Channel *c, int number);   // over the picture while watching
  void (*toast)(const char *msg, int line);       // short messages, top right
} Theme;

extern const Theme *THEMES[]; extern const int NTHEMES;
void themes_init(SDL_Renderer *r);
void draw_testcard(void);   // the test card (also shown by the TV engine on long waits)
