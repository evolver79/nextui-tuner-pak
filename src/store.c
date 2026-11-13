#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

char data_dir[512] = ".";

typedef struct { char key[64]; long when; } Entry;
typedef struct { Entry *v; int n; const char *file; } KeySet;
static KeySet favs = { 0, 0, "favourites.txt" }, hidden = { 0, 0, "hidden.txt" }, dead = { 0, 0, "dead.txt" };

static void path(char *out, size_t size, const char *file) { snprintf(out, size, "%s/%s", data_dir, file); }

static void set_load(KeySet *s) {
  char p[600]; path(p, sizeof p, s->file);
  FILE *f = fopen(p, "r"); if (!f) return;
  char line[160];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) continue;
    s->v = realloc(s->v, (s->n + 1) * sizeof *s->v);
    char *tab = strchr(line, '\t');
    s->v[s->n].when = tab ? atol(tab + 1) : 0;
    if (tab) *tab = 0;
    snprintf(s->v[s->n].key, sizeof s->v[s->n].key, "%s", line);
    s->n++;
  }
  fclose(f);
}

static void set_save(KeySet *s) {
  char p[600]; path(p, sizeof p, s->file);
  FILE *f = fopen(p, "w"); if (!f) return;
  for (int i = 0; i < s->n; i++) fprintf(f, "%s\t%ld\n", s->v[i].key, s->v[i].when);
  fclose(f);
}

static int set_find(KeySet *s, const char *key) {
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i].key, key)) return i;
  return -1;
}

static void set_toggle(KeySet *s, const char *key) {
  int i = set_find(s, key);
  if (i >= 0) s->v[i] = s->v[--s->n];
  else { s->v = realloc(s->v, (s->n + 1) * sizeof *s->v); snprintf(s->v[s->n].key, sizeof s->v[s->n].key, "%s", key); s->v[s->n].when = time(NULL); s->n++; }
  set_save(s);
}

void store_init(const char *dir) {
  snprintf(data_dir, sizeof data_dir, "%s", dir);
  mkdir(data_dir, 0755);
  char p[600]; path(p, sizeof p, "cache"); mkdir(p, 0755);
  set_load(&favs); set_load(&hidden); set_load(&dead);
}

void settings_defaults(Settings *s) {
  memset(s, 0, sizeof *s);
  snprintf(s->languages, sizeof s->languages, "eng");
  s->genres = (1u << G_COUNT) - 1;
  s->genres &= ~((1u << G_RELIGIOUS) | (1u << G_SHOP) | (1u << G_LEGISLATIVE) | (1u << G_ADULT));
  s->max_height = 720;
}

void settings_load(Settings *s) {
  settings_defaults(s);
  char p[600]; path(p, sizeof p, "settings.ini");
  FILE *f = fopen(p, "r"); if (!f) return;
  char line[1200];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '='); if (!eq) continue;
    *eq = 0; const char *k = line, *v = eq + 1;
    if (!strcmp(k, "languages")) snprintf(s->languages, sizeof s->languages, "%s", v);
    else if (!strcmp(k, "countries")) snprintf(s->countries, sizeof s->countries, "%s", v);
    else if (!strcmp(k, "genres")) s->genres = (unsigned)strtoul(v, NULL, 16);
    else if (!strcmp(k, "adult")) s->adult = atoi(v);
    else if (!strcmp(k, "max_height")) s->max_height = atoi(v);
    else if (!strcmp(k, "overlay")) s->overlay = atoi(v);
    else if (!strcmp(k, "theme")) s->theme = atoi(v);
    else if (!strcmp(k, "plain_tuning")) s->plain_tuning = atoi(v);
    else if (!strcmp(k, "mine_az")) s->mine_az = atoi(v);
    else if (!strcmp(k, "free_off")) s->free_off = atoi(v);
    else if (!strcmp(k, "pin")) snprintf(s->pin, sizeof s->pin, "%s", v);
    else if (!strcmp(k, "last_channel")) snprintf(s->last_channel, sizeof s->last_channel, "%s", v);
    else if (!strcmp(k, "setup_done")) s->setup_done = atoi(v);
    else if (!strcmp(k, "custom_m3u")) snprintf(s->custom_m3u, sizeof s->custom_m3u, "%s", v);
    else if (!strcmp(k, "custom_xmltv")) snprintf(s->custom_xmltv, sizeof s->custom_xmltv, "%s", v);
  }
  fclose(f);
}

void settings_save(const Settings *s) {
  char p[600]; path(p, sizeof p, "settings.ini");
  FILE *f = fopen(p, "w"); if (!f) return;
  fprintf(f, "languages=%s\ncountries=%s\ngenres=%x\nadult=%d\nmax_height=%d\noverlay=%d\ntheme=%d\nplain_tuning=%d\nmine_az=%d\nfree_off=%d\npin=%s\nlast_channel=%s\nsetup_done=%d\ncustom_m3u=%s\ncustom_xmltv=%s\n",
          s->languages, s->countries, s->genres, s->adult, s->max_height, s->overlay, s->theme, s->plain_tuning, s->mine_az, s->free_off, s->pin, s->last_channel, s->setup_done, s->custom_m3u, s->custom_xmltv);
  fclose(f);
}

int list_has(const char *csv, const char *item) {
  size_t n = strlen(item);
  for (const char *p = csv; (p = strstr(p, item)); p += n)
    if ((p == csv || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
  return 0;
}

void list_toggle(char *csv, size_t size, const char *item) {
  char out[512] = ""; int found = 0;
  char tmp[512]; snprintf(tmp, sizeof tmp, "%s", csv);
  for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
    if (!strcmp(t, item)) { found = 1; continue; }
    if (out[0]) strncat(out, ",", sizeof out - strlen(out) - 1);
    strncat(out, t, sizeof out - strlen(out) - 1);
  }
  if (!found) { if (out[0]) strncat(out, ",", sizeof out - strlen(out) - 1); strncat(out, item, sizeof out - strlen(out) - 1); }
  snprintf(csv, size, "%s", out);
}

int is_fav(const char *key) { return set_find(&favs, key) >= 0; }
void toggle_fav(const char *key) { set_toggle(&favs, key); }
int is_hidden(const char *key) { return set_find(&hidden, key) >= 0; }
void toggle_hidden(const char *key) { set_toggle(&hidden, key); }
int fav_count(void) { return favs.n; }
void unhide_all(void) { hidden.n = 0; set_save(&hidden); dead.n = 0; set_save(&dead); }

int is_dead(const char *key) {
  int i = set_find(&dead, key);
  return i >= 0 && time(NULL) - dead.v[i].when < 7 * 24 * 3600;
}

void mark_dead(const char *key) {
  int i = set_find(&dead, key);
  if (i < 0) { dead.v = realloc(dead.v, (dead.n + 1) * sizeof *dead.v); i = dead.n++; snprintf(dead.v[i].key, sizeof dead.v[i].key, "%s", key); }
  dead.v[i].when = time(NULL);
  set_save(&dead);
}
