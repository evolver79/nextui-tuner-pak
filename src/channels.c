#include "channels.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

const char *GENRE_NAMES[G_COUNT] = {
  "News", "Sports", "Movies", "Series", "Entertainment", "Kids", "Music", "Documentary", "Comedy",
  "Lifestyle", "Classic", "General", "Religious", "Shopping", "Legislative", "Adult"
};

// Keywords (lower case, any language the services use) -> genre. First match wins, so specific ones go first.
static const struct { const char *kw; int genre; } GENRE_KEYS[] = {
  {"xxx", G_ADULT}, {"adult", G_ADULT}, {"erotic", G_ADULT},
  {"news", G_NEWS}, {"nyhet", G_NEWS}, {"nachricht", G_NEWS}, {"notici", G_NEWS}, {"actualit", G_NEWS}, {"nyheder", G_NEWS},
  {"nieuws", G_NEWS}, {"weather", G_NEWS}, {"business", G_NEWS},
  {"sport", G_SPORTS}, {"deporte", G_SPORTS}, {"outdoor", G_SPORTS}, {"auto", G_SPORTS},
  {"kid", G_KIDS}, {"child", G_KIDS}, {"barn", G_KIDS}, {"kinder", G_KIDS}, {"infantil", G_KIDS}, {"enfant", G_KIDS},
  {"family", G_KIDS}, {"famil", G_KIDS}, {"animation", G_KIDS}, {"cartoon", G_KIDS}, {"anime", G_KIDS},
  {"movie", G_MOVIES}, {"film", G_MOVIES}, {"cine", G_MOVIES}, {"kino", G_MOVIES}, {"pel\xc3\xad", G_MOVIES},
  {"music", G_MUSIC}, {"musik", G_MUSIC}, {"m\xc3\xbasica", G_MUSIC}, {"musique", G_MUSIC}, {"mtv", G_MUSIC},
  {"docu", G_DOCUMENTARY}, {"dokument", G_DOCUMENTARY}, {"nature", G_DOCUMENTARY}, {"natur", G_DOCUMENTARY},
  {"science", G_DOCUMENTARY}, {"history", G_DOCUMENTARY}, {"histor", G_DOCUMENTARY}, {"education", G_DOCUMENTARY},
  {"culture", G_DOCUMENTARY}, {"kultur", G_DOCUMENTARY}, {"true crime", G_DOCUMENTARY},
  {"comed", G_COMEDY}, {"komedi", G_COMEDY}, {"humor", G_COMEDY}, {"sitcom", G_COMEDY}, {"south park", G_COMEDY},
  {"lifestyle", G_LIFESTYLE}, {"livsstil", G_LIFESTYLE}, {"food", G_LIFESTYLE}, {"cook", G_LIFESTYLE}, {"mat ", G_LIFESTYLE},
  {"travel", G_LIFESTYLE}, {"reise", G_LIFESTYLE}, {"home", G_LIFESTYLE}, {"relax", G_LIFESTYLE},
  {"classic", G_CLASSIC}, {"klassik", G_CLASSIC}, {"retro", G_CLASSIC}, {"godbit", G_CLASSIC}, {"gamle", G_CLASSIC},
  {"relig", G_RELIGIOUS}, {"faith", G_RELIGIOUS}, {"church", G_RELIGIOUS}, {"gospel", G_RELIGIOUS}, {"christ", G_RELIGIOUS},
  {"shop", G_SHOP}, {"legislat", G_LEGISLATIVE}, {"parliament", G_LEGISLATIVE}, {"government", G_LEGISLATIVE},
  {"public", G_GENERAL}, {"general", G_GENERAL},
  {"series", G_SERIES}, {"serie", G_SERIES}, {"drama", G_SERIES}, {"sci-fi", G_SERIES}, {"star trek", G_SERIES},
  {"overnaturlig", G_SERIES}, {"crime", G_SERIES}, {"krim", G_SERIES},
  {"reality", G_ENTERTAINMENT}, {"entertain", G_ENTERTAINMENT}, {"underhold", G_ENTERTAINMENT}, {"show", G_ENTERTAINMENT},
};

int genre_of(const char *group) {
  char low[160]; size_t i = 0;
  for (; group && group[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)group[i]);
  low[i] = 0;
  for (size_t k = 0; k < sizeof GENRE_KEYS / sizeof GENRE_KEYS[0]; k++) if (strstr(low, GENRE_KEYS[k].kw)) return GENRE_KEYS[k].genre;
  return G_ENTERTAINMENT;
}

Channel *channel_add(ChannelList *l) {
  if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = realloc(l->v, l->cap * sizeof *l->v); }
  Channel *c = &l->v[l->n++];
  memset(c, 0, sizeof *c);
  return c;
}

void channel_list_free(ChannelList *l) {
  for (int i = 0; i < l->n; i++) free(l->v[i].progs);
  free(l->v); memset(l, 0, sizeof *l);
}

static void attr(const char *line, const char *key, char *out, size_t size) {
  out[0] = 0;
  char pat[32]; snprintf(pat, sizeof pat, "%s=\"", key);
  const char *p = strstr(line, pat);
  if (!p) return;
  p += strlen(pat);
  const char *e = strchr(p, '"');
  if (!e) return;
  size_t n = (size_t)(e - p) < size - 1 ? (size_t)(e - p) : size - 1;
  memcpy(out, p, n); out[n] = 0;
}

static unsigned long hash_str(const char *s) { unsigned long h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }

int parse_m3u(const char *text, ChannelList *l, const char *source) {
  int added = 0;
  Channel cur = {0}; char group[160] = "";
  const char *p = text;
  while (*p) {
    const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
    char line[4096]; if (len >= sizeof line) len = sizeof line - 1;
    memcpy(line, p, len); line[len] = 0;
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    p = e ? e + 1 : p + strlen(p);
    if (!strncmp(line, "#EXTINF", 7)) {
      memset(&cur, 0, sizeof cur);
      attr(line, "tvg-id", cur.id, sizeof cur.id);
      attr(line, "tvg-logo", cur.logo, sizeof cur.logo);
      attr(line, "http-user-agent", cur.user_agent, sizeof cur.user_agent);
      attr(line, "group-title", group, sizeof group);
      const char *comma = strrchr(line, ',');
      if (comma && comma[1]) snprintf(cur.name, sizeof cur.name, "%s", comma + 1);
      // iptv-org names carry the quality and labels: "Channel (720p) [Geo-blocked]"
      char *q = strstr(cur.name, "p)");
      if (q) { char *o = q; while (o > cur.name && *o != '(') o--; if (*o == '(') cur.height = atoi(o + 1); }
      char *cut = strstr(cur.name, " ("); if (cut) *cut = 0;
      cut = strstr(cur.name, " ["); if (cut) *cut = 0;
      cur.genre = group[0] ? genre_of(group) : G_GENERAL;
      snprintf(cur.group, sizeof cur.group, "%s", group);
    } else if (!strncmp(line, "#EXTVLCOPT:http-user-agent=", 27)) {
      snprintf(cur.user_agent, sizeof cur.user_agent, "%s", line + 27);
    } else if (line[0] && line[0] != '#' && cur.name[0]) {
      Channel *c = channel_add(l);
      *c = cur;
      snprintf(c->url, sizeof c->url, "%s", line);
      snprintf(c->source, sizeof c->source, "%s", source);
      snprintf(c->key, sizeof c->key, "%s:%lx", source, hash_str(line));
      static int order = 0;
      c->order = order++;
      memset(&cur, 0, sizeof cur);
      added++;
    }
  }
  return added;
}

// "20261113203000 +0000" -> time_t (UTC)
static time_t xmltv_time(const char *s) {
  struct tm t = {0};
  if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) < 5) return 0;
  t.tm_year -= 1900; t.tm_mon -= 1;
  time_t utc = timegm(&t);
  const char *z = strchr(s, ' ');
  if (z && (z[1] == '+' || z[1] == '-')) {
    int hh = 0, mm = 0; sscanf(z + 2, "%2d%2d", &hh, &mm);
    int off = (hh * 60 + mm) * 60;
    utc += z[1] == '+' ? -off : off;
  }
  return utc;
}

static void unescape(char *s) {
  static const char *from[] = { "&amp;", "&lt;", "&gt;", "&quot;", "&apos;", "&#39;" };
  static const char to[] = { '&', '<', '>', '"', '\'', '\'' };
  for (int k = 0; k < 6; k++) {
    char *p;
    while ((p = strstr(s, from[k]))) { *p = to[k]; memmove(p + 1, p + strlen(from[k]), strlen(p + strlen(from[k])) + 1); }
  }
}

void add_programme(Channel *c, time_t start, time_t stop, const char *title, const char *icon) {
  c->progs = realloc(c->progs, (c->nprogs + 1) * sizeof *c->progs);
  Programme *p = &c->progs[c->nprogs++];
  memset(p, 0, sizeof *p);
  p->start = start; p->stop = stop;
  snprintf(p->title, sizeof p->title, "%s", title ? title : "");
  if (icon) snprintf(p->icon, sizeof p->icon, "%s", icon);
}

static int cmp_prog(const void *a, const void *b) {
  time_t x = ((const Programme *)a)->start, y = ((const Programme *)b)->start;
  return x < y ? -1 : x > y;
}

void sort_programmes(Channel *c) {
  if (!c->nprogs) return;
  qsort(c->progs, c->nprogs, sizeof *c->progs, cmp_prog);
  for (int i = 0; i < c->nprogs; i++)
    if (!c->progs[i].stop) c->progs[i].stop = i + 1 < c->nprogs ? c->progs[i + 1].start : c->progs[i].start + 3600;
}

static const char *NAME_NOISE[] = {
  "hd", "sd", "fhd", "uhd", "4k", "norway", "norge", "sweden", "sverige", "denmark", "danmark", "finland", "suomi",
  "uk", "unitedkingdom", "ireland", "germany", "deutschland", "austria", "switzerland", "netherlands", "belgium",
  "france", "spain", "espana", "portugal", "italy", "italia", "poland", "polska", "brazil", "brasil", "mexico",
  "argentina", "chile", "india", "korea", "japan", "australia", "newzealand", NULL };

void norm_name(const char *in, char *out, size_t size) {
  char words[256]; size_t w = 0; int depth = 0;
  // drop (...) and [...], keep words separated by single spaces, lower case
  for (const char *p = in; *p && w < sizeof words - 1; p++) {
    if (*p == '(' || *p == '[') { depth++; continue; }
    if (*p == ')' || *p == ']') { if (depth) depth--; continue; }
    if (depth) continue;
    unsigned char c = (unsigned char)*p;
    if (isalnum(c)) words[w++] = (char)tolower(c);
    else if (w && words[w - 1] != ' ') words[w++] = ' ';
  }
  words[w] = 0;
  size_t o = 0; out[0] = 0;
  char *save = NULL;
  for (char *t = strtok_r(words, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
    int noise = 0;
    for (int k = 0; NAME_NOISE[k]; k++) if (!strcmp(t, NAME_NOISE[k])) { noise = 1; break; }
    if (noise) continue;
    for (char *q = t; *q && o < size - 1; q++) out[o++] = *q;
  }
  out[o] = 0;
}

static Channel *find_by_id(Channel *chs, int n, const char *cid) {
  for (int i = 0; i < n; i++) {
    if (chs[i].epg_id[0] && !strcmp(chs[i].epg_id, cid)) return &chs[i];
    if (!chs[i].epg_id[0] && chs[i].id[0] && !strcmp(chs[i].id, cid)) return &chs[i];
  }
  return NULL;
}

int load_xmltv(const char *path, Channel *chs, int n, int by_name) {
  gzFile f = gzopen(path, "rb");   // reads plain files too
  if (!f) return 0;
  time_t now = time(NULL);
  int kept = 0;
  char (*norms)[64] = NULL;
  if (by_name) {   // names of channels still without a guide, normalised once
    norms = calloc(n, sizeof *norms);
    for (int i = 0; i < n; i++) if (!chs[i].nprogs && !chs[i].epg_id[0]) norm_name(chs[i].name, norms[i], sizeof norms[i]);
  }
  Channel *ch = NULL; Programme p = {0}; int in_prog = 0, in_prog_any = 0, have_title = 0;
  char cur_channel[128] = "";
  char line[8192];
  while (gzgets(f, line, sizeof line)) {
    char *t = strstr(line, "<programme");
    if (t) {
      char start[32], stop[32], cid[128];
      attr(t, "start", start, sizeof start); attr(t, "stop", stop, sizeof stop); attr(t, "channel", cid, sizeof cid);
      ch = find_by_id(chs, n, cid);
      memset(&p, 0, sizeof p);
      p.start = xmltv_time(start); p.stop = xmltv_time(stop);
      in_prog = ch && p.stop > now && p.start < now + 24 * 3600;
      in_prog_any = 1; have_title = 0; cur_channel[0] = 0;
    }
    char *cd = strstr(line, "<channel ");
    if (cd) attr(cd, "id", cur_channel, sizeof cur_channel);
    char *dn;
    if (by_name && cur_channel[0] && !in_prog_any && (dn = strstr(line, "<display-name"))) {
      char *gt = strchr(dn, '>'), *end = gt ? strstr(gt, "</display-name>") : NULL;
      if (gt && end) {
        char name[160]; size_t len = (size_t)(end - gt - 1) < sizeof name - 1 ? (size_t)(end - gt - 1) : sizeof name - 1;
        memcpy(name, gt + 1, len); name[len] = 0; unescape(name);
        char nn[64]; norm_name(name, nn, sizeof nn);
        if (nn[0]) for (int i = 0; i < n; i++)
          if (norms[i][0] && !strcmp(norms[i], nn)) { snprintf(chs[i].epg_id, sizeof chs[i].epg_id, "%s", cur_channel); norms[i][0] = 0; }
      }
    }
    char *ic = strstr(line, "<icon ");
    if (ic) {
      char src[256]; attr(ic, "src", src, sizeof src);
      if (in_prog_any) snprintf(p.icon, sizeof p.icon, "%s", src);
      else if (cur_channel[0])   // channel logos from the guide, for channels the playlist gave none
        for (int i = 0; i < n; i++)
          if (!chs[i].logo[0] && (!strcmp(chs[i].id, cur_channel) || !strcmp(chs[i].epg_id, cur_channel))) snprintf(chs[i].logo, sizeof chs[i].logo, "%s", src);
    }
    if (strstr(line, "</programme>")) {
      if (have_title && ch) { add_programme(ch, p.start, p.stop, p.title, p.icon); kept++; }
      in_prog = in_prog_any = have_title = 0;
    }
    char *ti;
    if (in_prog && !have_title && (ti = strstr(line, "<title"))) {
      char *gt = strchr(ti, '>'), *end = gt ? strstr(gt, "</title>") : NULL;
      if (gt && end) {
        size_t len = (size_t)(end - gt - 1) < sizeof p.title - 1 ? (size_t)(end - gt - 1) : sizeof p.title - 1;
        memcpy(p.title, gt + 1, len); p.title[len] = 0; unescape(p.title);
        have_title = 1;
      }
    }
  }
  gzclose(f);
  free(norms);
  for (int i = 0; i < n; i++) sort_programmes(&chs[i]);
  return kept;
}

void now_next(const Channel *c, time_t now, const Programme **cur, const Programme **next) {
  *cur = *next = NULL;
  for (int i = 0; i < c->nprogs; i++) {
    if (c->progs[i].start <= now && now < c->progs[i].stop) *cur = &c->progs[i];
    else if (c->progs[i].start > now) { *next = &c->progs[i]; break; }
  }
}
