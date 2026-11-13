// Channel logos for the lists: fetched in the background, cached on the card, kept as small textures.
#pragma once
#include <SDL2/SDL.h>
void logos_init(SDL_Renderer *r);
SDL_Texture *logo_get(const char *url);   // NULL until it has arrived (or when there is none); queues the fetch
void logos_pump(void);                    // main thread, once a frame: turns fetched images into textures
