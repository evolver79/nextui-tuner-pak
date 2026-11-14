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

// Programme lists carry a small header before the array: their capacity (they grow by doubling) and how many
// channels use them (channels with the same guide id share one list).
typedef struct { int refs, cap; long pad; } ProgHead;
static ProgHead *prog_head(Programme *p) { return (ProgHead *)p - 1; }
void progs_free(Programme *p) { if (p && --prog_head(p)->refs <= 0) free(prog_head(p)); }
static Programme *progs_share(Programme *p) { if (p) prog_head(p)->refs++; return p; }

void channel_list_free(ChannelList *l) {
  for (int i = 0; i < l->n; i++) progs_free(l->v[i].progs);
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
  ProgHead *h = c->progs ? prog_head(c->progs) : NULL;
  if (!h || h->refs > 1 || c->nprogs >= h->cap) {   // new, shared (copy before writing) or full: a bigger list of our own
    int cap = h && c->nprogs >= h->cap ? h->cap * 2 : h ? h->cap : 8;
    ProgHead *nh = malloc(sizeof *nh + (size_t)cap * sizeof *c->progs);
    nh->refs = 1; nh->cap = cap;
    if (c->nprogs) memcpy(nh + 1, c->progs, c->nprogs * sizeof *c->progs);
    if (h) progs_free(c->progs);
    c->progs = (Programme *)(nh + 1);
  }
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

// Channels by guide id (their name-matched epg_id, else their tvg-id): a hash table of the first channel with each id,
// and a chain from it to the others with the same id (a provider's SD/HD/FHD versions of one channel).
typedef struct { Channel *chs; int *slot, *next, mask; } IdIndex;
static const char *guide_key(const Channel *c) { return c->epg_id[0] ? c->epg_id : c->id; }
static int idx_find(const IdIndex *x, const char *id) {
  for (unsigned long h = hash_str(id) & x->mask;; h = (h + 1) & x->mask) {
    int i = x->slot[h];
    if (i < 0 || !strcmp(guide_key(&x->chs[i]), id)) return i;
  }
}
static void idx_add(IdIndex *x, int i) {
  const char *id = guide_key(&x->chs[i]);
  if (!id[0]) return;
  for (unsigned long h = hash_str(id) & x->mask;; h = (h + 1) & x->mask) {
    int j = x->slot[h];
    if (j < 0) { x->slot[h] = i; x->next[i] = -1; return; }
    if (j == i) return;
    if (!strcmp(guide_key(&x->chs[j]), id)) { x->next[i] = x->next[j]; x->next[j] = i; return; }
  }
}

time_t xmltv_last_stop;   // the end of the last programme in the guide read last (how far ahead it reaches)

// Reads an XMLTV guide (plain or gzip) element by element, whatever its line breaks (some providers write a 30 MB
// guide on a few hundred lines). Keeps programmes from now to 24 h ahead for the channels whose guide id matches;
// channels sharing an id share the list. by_name: first match channels without an id by their display names.
int load_xmltv(const char *path, Channel *chs, int n, int by_name) {
  xmltv_last_stop = 0;
  gzFile f = gzopen(path, "rb");   // reads plain files too
  if (!f) return 0;
  gzbuffer(f, 1 << 17);
  time_t now = time(NULL);
  IdIndex x = { chs, NULL, NULL, 0 };
  int cap = 1024; while (cap < n * 2) cap <<= 1;
  size_t nn = n > 0 ? (size_t)n : 1;
  x.mask = cap - 1; x.slot = malloc(cap * sizeof *x.slot); x.next = malloc(nn * sizeof *x.next);
  for (int i = 0; i < cap; i++) x.slot[i] = -1;
  for (int i = 0; i < n; i++) { x.next[i] = -1; idx_add(&x, i); }
  char (*norms)[64] = NULL;
  if (by_name) {   // names of channels still without a guide, normalised once
    norms = calloc(nn, sizeof *norms);
    for (int i = 0; i < n; i++) if (!chs[i].nprogs && !chs[i].epg_id[0]) norm_name(chs[i].name, norms[i], sizeof norms[i]);
  }
  enum { CAP_NONE, CAP_TITLE, CAP_NAME } capturing = CAP_NONE;
  char text[512]; size_t tlen = 0;
  Programme p = {0}; int ch = -1, in_prog = 0, have_title = 0, in_channel = 0, kept = 0;
  char cur_channel[128] = "";
  const size_t BUF = 1 << 18;
  char *buf = malloc(BUF + 1); size_t len = 0;
  for (;;) {
    int r = gzread(f, buf + len, (unsigned)(BUF - len));
    if (r > 0) len += (size_t)r;
    size_t pos = 0;
    for (;;) {
      char *lt = memchr(buf + pos, '<', len - pos);
      size_t text_end = lt ? (size_t)(lt - buf) : len;
      if (capturing && text_end > pos) {   // the text of a title or display-name
        size_t k = text_end - pos; if (k > sizeof text - 1 - tlen) k = sizeof text - 1 - tlen;
        memcpy(text + tlen, buf + pos, k); tlen += k;
      }
      if (!lt) { pos = len; break; }
      char *gt = memchr(lt, '>', len - (size_t)(lt - buf));
      if (!gt) { pos = (size_t)(lt - buf); break; }   // the rest of this tag comes with the next read
      *gt = 0;
      char *tag = lt + 1;
      pos = (size_t)(gt + 1 - buf);
      if (*tag == '?' || *tag == '!') continue;
      int closing = *tag == '/'; if (closing) tag++;
      size_t nl = strcspn(tag, " \t\r\n/");
      #define IS(name) (nl == sizeof name - 1 && !memcmp(tag, name, nl))
      if (IS("programme")) {
        if (!closing) {
          char start[32], stop[32], cid[128];
          attr(tag, "start", start, sizeof start); attr(tag, "stop", stop, sizeof stop); attr(tag, "channel", cid, sizeof cid);
          memset(&p, 0, sizeof p);
          p.start = xmltv_time(start); p.stop = xmltv_time(stop);
          if (p.stop > xmltv_last_stop) xmltv_last_stop = p.stop;
          ch = cid[0] ? idx_find(&x, cid) : -1;
          in_prog = ch >= 0 && p.stop > now && p.start < now + 24 * 3600;
          have_title = 0; in_channel = 0;
        } else {
          if (in_prog && have_title) { add_programme(&chs[ch], p.start, p.stop, p.title, p.icon[0] ? p.icon : NULL); kept++; }
          in_prog = 0; ch = -1;
        }
      } else if (IS("title")) {
        if (!closing && in_prog && !have_title) { capturing = CAP_TITLE; tlen = 0; }
        else if (closing && capturing == CAP_TITLE) {
          text[tlen] = 0; unescape(text); snprintf(p.title, sizeof p.title, "%s", text); have_title = 1; capturing = CAP_NONE;
        }
      } else if (IS("channel")) {
        if (!closing) { attr(tag, "id", cur_channel, sizeof cur_channel); in_channel = 1; }
        else { in_channel = 0; cur_channel[0] = 0; }
      } else if (IS("display-name")) {
        if (!closing && by_name && in_channel && cur_channel[0]) { capturing = CAP_NAME; tlen = 0; }
        else if (closing && capturing == CAP_NAME) {
          text[tlen] = 0; unescape(text); capturing = CAP_NONE;
          char nn[64]; norm_name(text, nn, sizeof nn);
          if (nn[0]) for (int i = 0; i < n; i++)
            if (norms[i][0] && !strcmp(norms[i], nn)) { snprintf(chs[i].epg_id, sizeof chs[i].epg_id, "%s", cur_channel); norms[i][0] = 0; idx_add(&x, i); }
        }
      } else if (IS("icon") && !closing) {
        char src[256]; attr(tag, "src", src, sizeof src);
        if (in_prog) snprintf(p.icon, sizeof p.icon, "%s", src);
        else if (in_channel && cur_channel[0] && src[0])   // channel logos from the guide, for channels the playlist gave none
          for (int i = idx_find(&x, cur_channel); i >= 0; i = x.next[i]) if (!chs[i].logo[0]) snprintf(chs[i].logo, sizeof chs[i].logo, "%s", src);
      }
      #undef IS
    }
    memmove(buf, buf + pos, len - pos); len -= pos;
    if (len == BUF) len = 0;   // one tag bigger than the buffer: not a guide we can use, skip it
    if (r <= 0) break;
  }
  gzclose(f);
  free(buf); free(norms);
  // sort, then share each list with the other channels that have the same id
  for (int h = 0; h < cap; h++) {
    int i = x.slot[h]; if (i < 0) continue;
    sort_programmes(&chs[i]);
    if (!chs[i].nprogs) continue;
    for (int j = x.next[i]; j >= 0; j = x.next[j])
      if (!chs[j].nprogs) { chs[j].progs = progs_share(chs[i].progs); chs[j].nprogs = chs[i].nprogs; }
  }
  free(x.slot); free(x.next);
  return kept;
}

void now_next(const Channel *c, time_t now, const Programme **cur, const Programme **next) {
  *cur = *next = NULL;
  for (int i = 0; i < c->nprogs; i++) {
    if (c->progs[i].start <= now && now < c->progs[i].stop) *cur = &c->progs[i];
    else if (c->progs[i].start > now) { *next = &c->progs[i]; break; }
  }
}
