// Subtitles that HLS carries beside the stream (EXT-X-MEDIA TYPE=SUBTITLES, WebVTT segments), which ffmpeg's hls
// demuxer leaves out. Their cues are timed on the stream's MPEG-TS clock (X-TIMESTAMP-MAP).
#pragma once
#include <stddef.h>

// (URL_MAX: addresses can carry long tokens: Pluto's subtitle playlists are ~2,400 characters with their jwt)
#define URL_MAX 4096
typedef struct { char lang[8], name[48], uri[URL_MAX]; } VttTrack;
typedef struct Vtt Vtt;

// The subtitle tracks a master playlist lists (one per language and name). Returns how many (0: none, or not HLS).
int vtt_find(const char *url, const char *user_agent, VttTrack *out, int max);
// Follows a track's playlist in a thread, keeping the cues around where it plays.
Vtt *vtt_start(const VttTrack *t, const char *user_agent);
void vtt_stop(Vtt *v);
// The text on at t (seconds on the stream's MPEG-TS clock), lines separated by \n; "" when none. Also tells the
// thread where playback is, so it fetches what's coming.
void vtt_text_at(Vtt *v, double t, char *out, size_t n);
