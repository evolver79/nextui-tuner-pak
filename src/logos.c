#include "logos.h"
#include "store.h"
#include <SDL2/SDL_image.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 2048
#define LOGO_W 96
#define LOGO_H 72
enum { L_NONE, L_QUEUED, L_SURFACE, L_READY, L_FAILED };
typedef struct { unsigned long hash; char url[256]; int state; SDL_Surface *surface; SDL_Texture *tex; } Slot;
static Slot slots[SLOTS];
static int queue[256], qhead, qcount;
static SDL_mutex *lock; static SDL_cond *cond;
static SDL_Renderer *ren;

static unsigned long hash_str(const char *s) { unsigned long h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }

static SDL_Surface *load_scaled(const char *path) {
  SDL_Surface *img = IMG_Load(path);
  if (!img) return NULL;
  double k = (double)LOGO_W / img->w; if (img->h * k > LOGO_H) k = (double)LOGO_H / img->h;
  int w = (int)(img->w * k), h = (int)(img->h * k); if (w < 1) w = 1; if (h < 1) h = 1;
  SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_Surface *conv = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ARGB8888, 0);
  SDL_FreeSurface(img);
  if (!out || !conv) { if (out) SDL_FreeSurface(out); if (conv) SDL_FreeSurface(conv); return NULL; }
  SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_NONE);
  SDL_BlitScaled(conv, NULL, out, NULL);
  SDL_FreeSurface(conv);
  return out;
}

static int worker(void *arg) {
  (void)arg;
  for (;;) {
    SDL_LockMutex(lock);
    while (!qcount) SDL_CondWait(cond, lock);
    int i = queue[qhead]; qhead = (qhead + 1) % 256; qcount--;
    char url[256]; snprintf(url, sizeof url, "%s", slots[i].url);
    SDL_UnlockMutex(lock);
    char path[700]; snprintf(path, sizeof path, "%s/cache/%lx.img", data_dir, hash_str(url));
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    else {   // download into the cache
      AVIOContext *io = NULL; AVDictionary *opt = NULL;
      av_dict_set(&opt, "rw_timeout", "6000000", 0);
      if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opt) >= 0) {
        FILE *o = fopen(path, "wb");
        unsigned char buf[16384]; int n, total = 0;
        while (o && (n = avio_read(io, buf, sizeof buf)) > 0 && total < 2 << 20) { fwrite(buf, 1, n, o); total += n; }
        if (o) fclose(o);
        avio_closep(&io);
      }
      av_dict_free(&opt);
    }
    SDL_Surface *s = load_scaled(path);
    SDL_LockMutex(lock);
    if (s) { slots[i].surface = s; slots[i].state = L_SURFACE; } else { slots[i].state = L_FAILED; remove(path); }
    SDL_UnlockMutex(lock);
  }
  return 0;
}

void logos_init(SDL_Renderer *r) {
  ren = r; lock = SDL_CreateMutex(); cond = SDL_CreateCond();
  for (int k = 0; k < 2; k++) SDL_DetachThread(SDL_CreateThread(worker, "logos", NULL));
}

SDL_Texture *logo_get(const char *url) {
  if (!url || !url[0]) return NULL;
  unsigned long h = hash_str(url);
  int i = (int)(h % SLOTS);
  for (int probe = 0; probe < 16; probe++, i = (i + 1) % SLOTS) {
    Slot *s = &slots[i];
    if (s->state != L_NONE && s->hash == h) return s->state == L_READY ? s->tex : NULL;
    if (s->state == L_NONE) {
      SDL_LockMutex(lock);
      if (qcount < 256) {
        s->hash = h; snprintf(s->url, sizeof s->url, "%s", url); s->state = L_QUEUED;
        queue[(qhead + qcount) % 256] = i; qcount++;
        SDL_CondSignal(cond);
      }
      SDL_UnlockMutex(lock);
      return NULL;
    }
  }
  return NULL;   // table crowded here: skip this logo
}

void logos_pump(void) {
  SDL_LockMutex(lock);
  for (int i = 0; i < SLOTS; i++) if (slots[i].state == L_SURFACE) {
    slots[i].tex = SDL_CreateTextureFromSurface(ren, slots[i].surface);
    SDL_FreeSurface(slots[i].surface); slots[i].surface = NULL;
    slots[i].state = slots[i].tex ? L_READY : L_FAILED;
  }
  SDL_UnlockMutex(lock);
}
