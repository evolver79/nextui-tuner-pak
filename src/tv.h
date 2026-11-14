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
int tv_nerd(char out[][96], int max);
void tv_audio_step(int dir, char *label, int n);   // LEFT/RIGHT while watching: the next audio track (remembered per channel)
void tv_hwdec(int on);             // Settings > Hardware decoding (takes effect on the next channel)   // stats for nerds: "LABEL|values" lines, returns how many
