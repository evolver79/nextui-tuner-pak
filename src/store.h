// Settings and the user's lists (favourites, hidden, dead channels), kept in the data folder.
#pragma once
#include "channels.h"

typedef struct {
  char languages[256];   // "eng,nor" (ISO 639-3, as iptv-org names its lists)
  char countries[256];   // "NO,GB"
  unsigned genres;       // bit per genre (1 << G_xxx): shown
  int adult;
  int max_height;        // 480 / 720 / 1080: skip iptv-org streams above it
  int overlay;
  int theme;
  int free_off;          // only your own playlists (no Pluto, Samsung, iptv-org, epgshare01)
  int mine_az;           // your channels A-Z instead of playlist order
  int plain_tuning;      // 1: no snow/picture/hiss while a channel loads, just the channel info and LOADING
  char pin[5];           // adult PIN, "" = none
  char last_channel[64]; // key of the channel watched last (Tuner starts on it)
  int setup_done;
  char custom_m3u[1024];   // extra playlists: URLs or files, separated by spaces
  char custom_xmltv[1024]; // their guides
  char pak_dir[512];       // not saved: where playlists.txt lives
} Settings;

extern char data_dir[512];
void store_init(const char *dir);
void settings_defaults(Settings *s);
void settings_load(Settings *s);
void settings_save(const Settings *s);
int list_has(const char *csv, const char *item);      // "eng,nor" contains "nor"
void list_toggle(char *csv, size_t size, const char *item);

// Favourites and hidden channels, by Channel.key. Dead channels: hidden for a week after the watchdog gives up.
int is_fav(const char *key);
void toggle_fav(const char *key);
int is_hidden(const char *key);
void toggle_hidden(const char *key);
int is_dead(const char *key);
void mark_dead(const char *key);
int fav_count(void);
void unhide_all(void);
