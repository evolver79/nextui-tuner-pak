#pragma once
#include <SDL2/SDL.h>
#include "channels.h"
#include "store.h"
// The TV engine: one channel at a time. tv_render() draws the picture (or snow + tuning image, or NO SIGNAL) and the
// overlay; call it every frame while a channel is tuned, then draw menus/banners on top.
enum { TV_OFF, TV_TUNING, TV_PLAYING, TV_NO_SIGNAL };
void tv_setup(SDL_Renderer *ren, SDL_AudioDeviceID audio, Settings *set);
void tv_tune(Channel *c);          // NULL switches off
Channel *tv_channel(void);
int tv_state(void);
Uint32 tv_tuned_ms(void);
void tv_render(void);
void tv_overlay_step(int dir);
const char *tv_overlay_name(void);
void tv_osd_line(char *buf, int n);
