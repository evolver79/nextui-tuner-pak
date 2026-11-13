#include "net.h"
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>
#include <SDL2/SDL.h>

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1);
  if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f); b[n] = 0; if (len) *len = n;
  return b;
}

static char *gunzip(const unsigned char *in, size_t inlen, size_t *outlen) {
  z_stream z = {0};
  if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return NULL;
  size_t cap = inlen * 8 + 1024, n = 0;
  char *out = malloc(cap);
  z.next_in = (unsigned char *)in; z.avail_in = inlen;
  int r;
  do {
    if (cap - n < 65536) { cap *= 2; out = realloc(out, cap); }
    z.next_out = (unsigned char *)out + n; z.avail_out = cap - n - 1;
    r = inflate(&z, Z_NO_FLUSH);
    n = cap - 1 - z.avail_out;
  } while (r == Z_OK);
  inflateEnd(&z);
  if (r != Z_STREAM_END) { free(out); return NULL; }
  out[n] = 0; *outlen = n;
  return out;
}

char *fetch(const char *url, const char *user_agent, const char *cache_path, long max_age, size_t *len) {
  struct stat st;
  int cached = cache_path && stat(cache_path, &st) == 0;
  if (cached && time(NULL) - st.st_mtime < max_age) return read_file(cache_path, len);

  AVIOContext *io = NULL; AVDictionary *opt = NULL;
  av_dict_set(&opt, "rw_timeout", "15000000", 0);
  if (user_agent && user_agent[0]) av_dict_set(&opt, "user_agent", user_agent, 0);
  char *buf = NULL; size_t n = 0, cap = 0;
  if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opt) >= 0) {
    unsigned char chunk[65536]; int r;
    while ((r = avio_read(io, chunk, sizeof chunk)) > 0) {
      if (n + r + 1 > cap) { cap = (n + r + 1) * 2; buf = realloc(buf, cap); }
      memcpy(buf + n, chunk, r); n += r;
    }
    avio_closep(&io);
  }
  av_dict_free(&opt);
  if (buf && n > 2 && (unsigned char)buf[0] == 0x1f && (unsigned char)buf[1] == 0x8b) {
    size_t un; char *u = gunzip((unsigned char *)buf, n, &un);
    free(buf); buf = u; n = u ? un : 0;
  }
  if (buf && n) {
    buf[n] = 0;
    if (cache_path) { FILE *f = fopen(cache_path, "wb"); if (f) { fwrite(buf, 1, n, f); fclose(f); } }
    if (len) *len = n;
    return buf;
  }
  free(buf);
  return cached ? read_file(cache_path, len) : NULL;   // offline: the last copy, however old
}

int fetch_to_file(const char *url, const char *user_agent, const char *path, long max_age) {
  struct stat st;
  int cached = stat(path, &st) == 0 && st.st_size > 0;
  if (cached && time(NULL) - st.st_mtime < max_age) return 1;
  AVIOContext *io = NULL; AVDictionary *opt = NULL;
  av_dict_set(&opt, "rw_timeout", "30000000", 0);
  if (user_agent && user_agent[0]) av_dict_set(&opt, "user_agent", user_agent, 0);
  int ok = 0;
  if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opt) >= 0) {
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
      unsigned char chunk[65536]; int r; long total = 0;
      while ((r = avio_read(io, chunk, sizeof chunk)) > 0) { fwrite(chunk, 1, r, f); total += r; }
      fclose(f);
      if (total > 0 && rename(tmp, path) == 0) ok = 1; else remove(tmp);
    }
    avio_closep(&io);
  }
  av_dict_free(&opt);
  return ok || cached;
}

typedef struct { char url[1024], ua[128], path[700]; volatile int *done; } AsyncJob;
static int async_thread(void *arg) {
  AsyncJob *j = arg;
  struct stat before; int had = stat(j->path, &before) == 0;
  int ok = fetch_to_file(j->url, j->ua, j->path, 0);
  struct stat after;
  if (ok && j->done && stat(j->path, &after) == 0 && (!had || after.st_mtime != before.st_mtime)) *j->done = 1;
  free(j);
  return 0;
}
void fetch_to_file_async(const char *url, const char *user_agent, const char *path, volatile int *done) {
  AsyncJob *j = calloc(1, sizeof *j);
  snprintf(j->url, sizeof j->url, "%s", url); snprintf(j->ua, sizeof j->ua, "%s", user_agent ? user_agent : "");
  snprintf(j->path, sizeof j->path, "%s", path); j->done = done;
  SDL_DetachThread(SDL_CreateThread(async_thread, "guide", j));
}
