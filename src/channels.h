// Channels, programmes and genres for Tuner.
#pragma once
#include <time.h>

typedef struct { time_t start, stop; char title[96]; char icon[256]; } Programme;

enum {
  G_NEWS, G_SPORTS, G_MOVIES, G_SERIES, G_ENTERTAINMENT, G_KIDS, G_MUSIC, G_DOCUMENTARY, G_COMEDY,
  G_LIFESTYLE, G_CLASSIC, G_GENERAL, G_RELIGIOUS, G_SHOP, G_LEGISLATIVE, G_ADULT, G_COUNT
};
extern const char *GENRE_NAMES[G_COUNT];
// Best genre for a category or group name in any of the usual languages ("Nyheter", "Kids", "Película"...).
int genre_of(const char *group);

typedef struct {
  char key[64];         // stable id (favourites, hidden): "plu:<id>", "stvp:<id>" or a hash of the url
  char name[96];
  char id[96];          // tvg-id: the channel's id in an XMLTV guide
  char epg_id[128];     // id in a guide matched by name (epgshare01), when it differs from tvg-id
  char logo[256];
  char art[256];        // a big picture for the tuning screen (Pluto's featured image)
  char user_agent[128];
  char url[1024];
  char source[16];      // "pluto", "samsung", "iptv-org", "custom"
  char country[4];
  int genre;
  char group[64];       // the playlist's own group-title (your playlists are browsed by it)
  int order;            // position in its playlist (playlist order for your channels)
  int height;           // stream height if known (iptv-org names say "(720p)"), else 0
  Programme *progs; int nprogs;   // sorted by start
} Channel;

typedef struct { Channel *v; int n, cap; } ChannelList;
Channel *channel_add(ChannelList *l);
void channel_list_free(ChannelList *l);

// Reads an M3U playlist (#EXTINF tvg-id/tvg-logo/group-title, #EXTVLCOPT:http-user-agent) into the list.
int parse_m3u(const char *text, ChannelList *l, const char *source);
// Adds programmes from an XMLTV file (plain or .gz) to channels whose tvg-id matches. Returns programmes kept.
// by_name: also match channels that have no programmes yet by their name (see norm_name) to the guide's
// display-name, for guides with their own ids (epgshare01).
int load_xmltv(const char *path, Channel *chs, int n, int by_name);
// "NRK1 HD Norway (NO)" -> "nrk1": lower case letters and digits, without brackets, HD/SD and country names.
void norm_name(const char *in, char *out, size_t size);
// Programme on air at `now` (or NULL) and the one after it (or NULL).
void now_next(const Channel *c, time_t now, const Programme **cur, const Programme **next);
// Adds a programme (stop 0 = until the next one starts; fixed by sort_programmes).
void add_programme(Channel *c, time_t start, time_t stop, const char *title, const char *icon);
void progs_free(Programme *p);   // programme lists are shared between channels with the same guide id
extern time_t xmltv_last_stop;   // set by load_xmltv: the end of the guide's last programme
void sort_programmes(Channel *c);
