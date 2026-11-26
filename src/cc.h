// Closed captions (CEA-608) carried in H.264/H.265 SEI (user data registered, ATSC "GA94"): read from the video
// packets themselves, so hardware decoding has them too; put in display order; decoded by ffmpeg's ccaption.
#pragma once
#include <libavcodec/avcodec.h>

// A caption: from `at` (seconds, the packets' clock) until `end` (or the next caption when end < 0). "" clears.
typedef void (*CcEmit)(void *ud, double at, double end, const char *text);
typedef struct CcState CcState;

CcState *cc_open(void);
void cc_close(CcState *c);
// One video packet (Annex B). Returns 1 when it carries caption data. With decode, captions come out through emit
// (a moment later: they're held to be put in display order); without, the decoder is reset and nothing is held.
int cc_packet(CcState *c, const AVPacket *pkt, AVRational tb, int hevc, int decode, CcEmit emit, void *ud);
