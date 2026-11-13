// Builds the channel list from the sources the settings ask for.
#pragma once
#include "channels.h"
#include "store.h"

typedef void (*ProgressFn)(const char *msg, void *ud);
// Pluto TV and Samsung TV Plus (i.mjh.nz: channels + guide) for the chosen countries, iptv-org playlists for the
// chosen languages and countries, then the custom playlists/guides. Deduplicated by stream URL, filtered by genre,
// adult and quality, sorted by genre then name. force = ignore the caches' age.
void sources_build(const Settings *s, ChannelList *out, int force, ProgressFn cb, void *ud);

typedef struct { const char *code, *name; } Choice;
extern const Choice LANGUAGES[]; extern const int NLANGUAGES;
extern const Choice COUNTRIES[]; extern const int NCOUNTRIES;
// "PLUTO SAMSUNG" etc. for a country code, for the setup screen.
const char *country_services(const char *code);
