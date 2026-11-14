// Hardware video decoding on the Allwinner video engine (the Brick's A133), through Allwinner's CedarC libraries in
// /usr/lib, loaded at run time. H.264 and H.265 (8-bit); everything else, or anything that fails, stays in software.
#pragma once
#include <libavcodec/avcodec.h>

typedef struct HwDec HwDec;
int hwdec_available(void);                                    // the libraries are there and load
// A decoder for this stream, or NULL (codec not supported, the engine busy for too long, or it failed to start).
HwDec *hwdec_open(const AVCodecParameters *par, AVRational time_base, double fps);
// Gives it one packet: 0 taken, AVERROR(EAGAIN) no room yet (take pictures first), <0 the decoder failed.
int hwdec_send(HwDec *d, const AVPacket *pkt);
// The next decoded picture as a yuv420p frame (pts in microseconds), or NULL for now. *err is set when it failed.
AVFrame *hwdec_receive(HwDec *d, int *err);
void hwdec_close(HwDec *d);
