#include "sources.h"
#include "net.h"
#include "vendor/cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const Choice LANGUAGES[] = {
  {"eng", "English"}, {"nor", "Norsk"}, {"swe", "Svenska"}, {"dan", "Dansk"}, {"fin", "Suomi"}, {"isl", "Islenska"},
  {"deu", "Deutsch"}, {"nld", "Nederlands"}, {"fra", "Francais"}, {"spa", "Espanol"}, {"por", "Portugues"},
  {"ita", "Italiano"}, {"pol", "Polski"}, {"ces", "Cestina"}, {"hun", "Magyar"}, {"ron", "Romana"}, {"ell", "Ellinika"},
  {"tur", "Turkce"}, {"rus", "Russkij"}, {"ukr", "Ukrainska"}, {"ara", "Arabic"}, {"hin", "Hindi"}, {"jpn", "Japanese"},
  {"kor", "Korean"}, {"zho", "Chinese"},
};
const int NLANGUAGES = sizeof LANGUAGES / sizeof LANGUAGES[0];

const Choice COUNTRIES[] = {
  {"NO", "Norway"}, {"SE", "Sweden"}, {"DK", "Denmark"}, {"FI", "Finland"}, {"IS", "Iceland"}, {"GB", "United Kingdom"},
  {"IE", "Ireland"}, {"US", "United States"}, {"CA", "Canada"}, {"AU", "Australia"}, {"NZ", "New Zealand"},
  {"DE", "Germany"}, {"AT", "Austria"}, {"CH", "Switzerland"}, {"NL", "Netherlands"}, {"BE", "Belgium"},
  {"FR", "France"}, {"ES", "Spain"}, {"PT", "Portugal"}, {"IT", "Italy"}, {"PL", "Poland"}, {"BR", "Brazil"},
  {"MX", "Mexico"}, {"AR", "Argentina"}, {"CL", "Chile"}, {"IN", "India"}, {"KR", "South Korea"}, {"JP", "Japan"},
};
const int NCOUNTRIES = sizeof COUNTRIES / sizeof COUNTRIES[0];

// Regions i.mjh.nz has for each service (checked Nov 2026).
static const char *PLUTO_REGIONS = "us,gb,es,ca,de,br,mx,cl,fr,it,no,se,dk,ar";
static const char *SAMSUNG_REGIONS = "at,ca,ch,de,es,fr,gb,in,it,kr,us";

char sources_warning[160];

static int in_csv(const char *csv, const char *item) {
  size_t n = strlen(item);
  for (const char *p = csv; (p = strstr(p, item)); p += n)
    if ((p == csv || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
  return 0;
}

const char *country_services(const char *code) {
  static char out[32]; char low[4] = { (char)tolower(code[0]), (char)tolower(code[1]), 0 };
  out[0] = 0;
  if (in_csv(PLUTO_REGIONS, low)) strcat(out, "PLUTO ");
  if (in_csv(SAMSUNG_REGIONS, low)) strcat(out, "SAMSUNG");
  return out;
}

static void cache_path(char *out, size_t size, const char *name) { snprintf(out, size, "%s/cache/%s", data_dir, name); }

// i.mjh.nz ".channels.json.gz": { regions: { no: { channels: { <id>: { name, group, logo, art, programs: [[start, title]...],
// license_url? } } } } }
static int add_mjh(const char *service, const char *regions_csv, const char *countries, const char *url_fmt, const char *ua,
                   const char *source, const char *key_prefix, ChannelList *out, long max_age, ProgressFn cb, void *ud) {
  char want[128] = "";
  char tmp[256]; snprintf(tmp, sizeof tmp, "%s", countries);
  for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
    char low[4] = { (char)tolower(t[0]), (char)tolower(t[1]), 0 };
    if (in_csv(regions_csv, low)) { if (want[0]) strcat(want, ","); strcat(want, low); }
  }
  if (!want[0]) return 0;
  char msg[128]; snprintf(msg, sizeof msg, "Fetching %s...", service); cb(msg, ud);
  char url[256], cp[600], name[64];
  snprintf(url, sizeof url, "https://i.mjh.nz/%s/.channels.json.gz", service);
  snprintf(name, sizeof name, "%s.json", source); cache_path(cp, sizeof cp, name);
  char *json = fetch(url, NULL, cp, max_age, NULL);
  if (!json) return 0;
  cJSON *root = cJSON_Parse(json); free(json);
  if (!root) return 0;
  int added = 0;
  cJSON *regions = cJSON_GetObjectItem(root, "regions");
  char wtmp[128]; snprintf(wtmp, sizeof wtmp, "%s", want);
  for (char *r = strtok(wtmp, ","); r; r = strtok(NULL, ",")) {
    cJSON *chs = cJSON_GetObjectItem(cJSON_GetObjectItem(regions, r), "channels");
    cJSON *it;
    cJSON_ArrayForEach(it, chs) {
      if (cJSON_GetObjectItem(it, "license_url")) continue;   // DRM: can't be played
      Channel *c = channel_add(out);
      const char *v;
      snprintf(c->key, sizeof c->key, "%s:%s", key_prefix, it->string);
      snprintf(c->url, sizeof c->url, url_fmt, it->string);
      if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name")))) snprintf(c->name, sizeof c->name, "%s", v);
      if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "logo")))) snprintf(c->logo, sizeof c->logo, "%s", v);
      if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "art")))) snprintf(c->art, sizeof c->art, "%s", v);
      v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "group"));
      c->genre = genre_of(v ? v : "");
      if (ua) snprintf(c->user_agent, sizeof c->user_agent, "%s", ua);
      snprintf(c->source, sizeof c->source, "%s", source);
      c->country[0] = (char)toupper(r[0]); c->country[1] = (char)toupper(r[1]);
      cJSON *p;
      cJSON_ArrayForEach(p, cJSON_GetObjectItem(it, "programs")) {
        cJSON *st = cJSON_GetArrayItem(p, 0), *ti = cJSON_GetArrayItem(p, 1);
        if (cJSON_IsNumber(st) && cJSON_IsString(ti)) add_programme(c, (time_t)st->valuedouble, 0, ti->valuestring, NULL);
      }
      sort_programmes(c);
      added++;
    }
  }
  cJSON_Delete(root);
  return added;
}


// Pluto/Samsung channels that came in through iptv-org (other regions than the chosen ones, e.g. Pluto US in the
// English list) get their guide from the same i.mjh.nz data, found by the id in their jmp2.uk URL.
typedef struct { char id[48]; Channel *c; } Need;
static int cmp_need(const void *a, const void *b) { return strcmp(((const Need *)a)->id, ((const Need *)b)->id); }
static void fill_mjh_guides(const char *service, const char *cache_name, const char *marker, ChannelList *all, long max_age, ProgressFn cb, void *ud) {
  Need *need = NULL; int n = 0;
  for (int i = 0; i < all->n; i++) {
    Channel *c = &all->v[i];
    if (c->nprogs) continue;
    const char *m = strstr(c->url, marker);
    if (!m) continue;
    m += strlen(marker);
    need = realloc(need, (n + 1) * sizeof *need);
    size_t k = 0; while (m[k] && m[k] != '.' && m[k] != '?' && m[k] != '/' && k < sizeof need[n].id - 1) { need[n].id[k] = m[k]; k++; }
    need[n].id[k] = 0; need[n].c = c; n++;
  }
  if (!n) { free(need); return; }
  qsort(need, n, sizeof *need, cmp_need);
  char msg[96]; snprintf(msg, sizeof msg, "Guide for %d %s channels...", n, service); cb(msg, ud);
  char url[256], cp[600];
  snprintf(url, sizeof url, "https://i.mjh.nz/%s/.channels.json.gz", service);
  cache_path(cp, sizeof cp, cache_name);
  char *json = fetch(url, NULL, cp, max_age, NULL);
  if (!json) { free(need); return; }
  cJSON *root = cJSON_Parse(json); free(json);
  cJSON *region;
  cJSON_ArrayForEach(region, cJSON_GetObjectItem(root, "regions")) {
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(region, "channels")) {
      Need key; snprintf(key.id, sizeof key.id, "%s", it->string);
      Need *hit = bsearch(&key, need, n, sizeof *need, cmp_need);
      if (!hit || hit->c->nprogs) continue;
      cJSON *p;
      cJSON_ArrayForEach(p, cJSON_GetObjectItem(it, "programs")) {
        cJSON *st = cJSON_GetArrayItem(p, 0), *ti = cJSON_GetArrayItem(p, 1);
        if (cJSON_IsNumber(st) && cJSON_IsString(ti)) add_programme(hit->c, (time_t)st->valuedouble, 0, ti->valuestring, NULL);
      }
      sort_programmes(hit->c);
      const char *v;
      if (!hit->c->art[0] && (v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "art")))) snprintf(hit->c->art, sizeof hit->c->art, "%s", v);
    }
  }
  cJSON_Delete(root);
  free(need);
}

static int add_m3u_url(const char *url, const char *cache_name, const char *source, ChannelList *out, long max_age) {
  char cp[600]; cache_path(cp, sizeof cp, cache_name);
  char *text = fetch(url, NULL, cp, max_age, NULL);
  if (!text) return 0;
  int n = parse_m3u(text, out, source);
  free(text);
  return n;
}

static int cmp_channel(const void *a, const void *b) {
  const Channel *x = a, *y = b;
  if (x->genre != y->genre) return x->genre - y->genre;
  return strcasecmp(x->name, y->name);
}

void sources_build(const Settings *s, ChannelList *out, int force, ProgressFn cb, void *ud) {
  ChannelList all = {0};
  long guide_age = force ? 0 : 6 * 3600, list_age = force ? 0 : 24 * 3600;
  char tmp[256], url[256], name[64], msg[96];
  if (!s->free_off) {
  add_mjh("PlutoTV", PLUTO_REGIONS, s->countries, "https://jmp2.uk/plu-%s.m3u8", NULL, "pluto", "plu", &all, guide_age, cb, ud);
  add_mjh("SamsungTVPlus", SAMSUNG_REGIONS, s->countries, "https://jmp2.uk/stvp-%s", "okhttp/4.12.0", "samsung", "stvp", &all, guide_age, cb, ud);

  snprintf(tmp, sizeof tmp, "%s", s->languages);
  for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
    snprintf(msg, sizeof msg, "Fetching iptv-org: %s...", t); cb(msg, ud);
    snprintf(url, sizeof url, "https://iptv-org.github.io/iptv/languages/%s.m3u", t);
    snprintf(name, sizeof name, "iptv-lang-%s.m3u", t);
    add_m3u_url(url, name, "iptv", &all, list_age);
  }
  snprintf(tmp, sizeof tmp, "%s", s->countries);
  for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
    char low[4] = { (char)tolower(t[0]), (char)tolower(t[1]), 0 };
    snprintf(msg, sizeof msg, "Fetching iptv-org: %s...", t); cb(msg, ud);
    snprintf(url, sizeof url, "https://iptv-org.github.io/iptv/countries/%s.m3u", low);
    snprintf(name, sizeof name, "iptv-country-%s.m3u", low);
    add_m3u_url(url, name, "iptv", &all, list_age);
  }



  // Guides for national channels from epgshare01 (one XMLTV per country, its own channel ids): matched by name to
  // the iptv-org channels that have no guide yet.
  static const struct { const char *cc, *file; } EPGSHARE[] = {
    {"NO","NO1"},{"SE","SE1"},{"DK","DK1"},{"FI","FI1"},{"GB","UK1"},{"IE","IE1"},{"AU","AU1"},{"NZ","NZ1"},{"DE","DE1"},
    {"AT","AT1"},{"CH","CH1"},{"NL","NL1"},{"BE","BE2"},{"FR","FR1"},{"ES","ES1"},{"PT","PT1"},{"IT","IT1"},{"PL","PL1"},
    {"BR","BR1"},{"MX","MX1"},{"AR","AR1"},{"CL","CL1"},{"IN","IN1"},{"KR","KR1"},{"JP","JP1"} };
  for (size_t e = 0; e < sizeof EPGSHARE / sizeof EPGSHARE[0]; e++) {
    if (!list_has(s->countries, EPGSHARE[e].cc)) continue;
    snprintf(msg, sizeof msg, "Fetching TV guide: %s...", EPGSHARE[e].cc); cb(msg, ud);
    char p[700]; snprintf(name, sizeof name, "epgshare-%s.xml", EPGSHARE[e].file); cache_path(p, sizeof p, name);
    snprintf(url, sizeof url, "https://epgshare01.online/epgshare01/epg_ripper_%s.xml.gz", EPGSHARE[e].file);
    char *x = fetch(url, NULL, p, force ? 0 : 12 * 3600, NULL);
    if (!x) continue;
    free(x);
    // iptv-org channels only (Pluto/Samsung have their own guides)
    int first = -1, last = -1;
    for (int i = 0; i < all.n; i++) if (!strcmp(all.v[i].source, "iptv")) { if (first < 0) first = i; last = i; }
    if (first >= 0) { int got = load_xmltv(p, all.v + first, last - first + 1, 1); fprintf(stderr, "epgshare %s: %d programmes\n", EPGSHARE[e].file, got); }
  }

  }   // free sources

  // Your own playlists and guides: playlists.txt in the pak folder (one per line: an M3U URL or file name, "epg <url
  // or file>" for an XMLTV guide, # for comments), plus custom_m3u=/custom_xmltv= in settings.ini.
  int first_custom = all.n;
  char lists[4096] = "", guides[4096] = "";
  snprintf(lists, sizeof lists, "%s", s->custom_m3u);
  snprintf(guides, sizeof guides, "%s", s->custom_xmltv);
  char pl[700]; snprintf(pl, sizeof pl, "%s/playlists.txt", s->pak_dir);
  FILE *pf = fopen(pl, "r");
  if (pf) {
    char line[1024];
    while (fgets(line, sizeof line, pf)) {
      line[strcspn(line, "\r\n")] = 0;
      char *t = line; while (*t == ' ' || *t == '\t') t++;
      if (!*t || *t == '#') continue;
      int epg = !strncmp(t, "epg ", 4);
      if (epg) { t += 4; while (*t == ' ') t++; }
      char *dst = epg ? guides : lists;
      if (dst[0]) strncat(dst, " ", 4095 - strlen(dst));
      strncat(dst, t, 4095 - strlen(dst));
    }
    fclose(pf);
  }
  int k = 0, failed = 0;
  sources_warning[0] = 0;
  char *save = NULL;
  for (char *t = strtok_r(lists, " ", &save); t; t = strtok_r(NULL, " ", &save), k++) {
    cb("Fetching your playlists...", ud);
    if (!strncmp(t, "http", 4)) {
      snprintf(name, sizeof name, "custom-%d.m3u", k);
      if (!add_m3u_url(t, name, "custom", &all, list_age)) { failed++; fprintf(stderr, "playlist %d: no channels (download failed or not an M3U)\n", k + 1); }
    }
    else {
      char p[1200]; snprintf(p, sizeof p, t[0] == '/' ? "%s" : "%s/%s", t[0] == '/' ? t : s->pak_dir, t);
      FILE *f = fopen(p, "rb");
      if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *text = malloc(n + 1);
        if (fread(text, 1, n, f) == (size_t)n) { text[n] = 0; parse_m3u(text, &all, "custom"); }
        free(text); fclose(f);
      } else { failed++; fprintf(stderr, "playlist not found: %s\n", p); }
    }
  }
  if (failed) snprintf(sources_warning, sizeof sources_warning, "%d of your playlists gave no channels (download failed?)", failed);
  k = 0; save = NULL;
  for (char *t = strtok_r(guides, " ", &save); t; t = strtok_r(NULL, " ", &save), k++) {
    cb("Fetching your guides...", ud);
    char p[1200];
    if (!strncmp(t, "http", 4)) {
      snprintf(name, sizeof name, "custom-%d.xml", k); cache_path(p, sizeof p, name);
      char *x = fetch(t, NULL, p, guide_age, NULL); free(x);
    } else snprintf(p, sizeof p, t[0] == '/' ? "%s" : "%s/%s", t[0] == '/' ? t : s->pak_dir, t);
    load_xmltv(p, all.v + first_custom, all.n - first_custom, 0);
  }
  // Pluto/Samsung channels in other lists (iptv-org, your playlists) get the services' guides by their id.
  fill_mjh_guides("PlutoTV", "pluto.json", "jmp2.uk/plu-", &all, guide_age, cb, ud);
  fill_mjh_guides("SamsungTVPlus", "samsung.json", "jmp2.uk/stvp-", &all, guide_age, cb, ud);

  // Your channels are never filtered by genre or quality (you asked for them), only deduplicated.
  for (int i = first_custom; i < all.n; i++) all.v[i].height = 0;

  // Keep: allowed genre, adult only if asked, quality within the cap, and the first stream for each URL.
  cb("Sorting channels...", ud);
  // A hash set of the URLs kept so far, so big playlists deduplicate quickly.
  size_t hcap = 1; while (hcap < (size_t)all.n * 2 + 16) hcap <<= 1;
  const char **seen = calloc(hcap, sizeof *seen);
  // Your channels go first, so a stream that is both in your playlist and in another list stays yours.
  for (int pass = 0; pass < 2; pass++)
  for (int i = 0; i < all.n; i++) {
    Channel *c = &all.v[i];
    int mine = !strcmp(c->source, "custom");
    if (mine != (pass == 0)) continue;
    int ok = mine || (s->genres & (1u << c->genre)) || (c->genre == G_ADULT && s->adult);
    if (c->genre == G_ADULT && !s->adult && !mine) ok = 0;
    if (c->height > s->max_height) ok = 0;
    size_t h = 5381; for (const char *q = c->url; *q; q++) h = h * 33 + (unsigned char)*q;
    size_t slot = h & (hcap - 1);
    while (ok && seen[slot]) { if (!strcmp(seen[slot], c->url)) ok = 0; else slot = (slot + 1) & (hcap - 1); }
    if (ok) seen[slot] = c->url;
    if (ok) { Channel *d = channel_add(out); *d = *c; c->progs = NULL; }
    else free(c->progs);
    c->progs = NULL;
  }
  free(seen);
  free(all.v);
  qsort(out->v, out->n, sizeof *out->v, cmp_channel);
  snprintf(msg, sizeof msg, "%d channels", out->n); cb(msg, ud);
}
