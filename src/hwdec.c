// Hardware video decoding with Allwinner's CedarC (libvdecoder.so and friends, as installed on the TrimUI Brick).
// The structs below follow CedarC 1.3.0 as built for the Brick (the November 2020 layout). That was checked against
// the library's own code: InitializeVideoDecoder reads memops at byte 136, the VE ops at 152 and proc info at 180,
// and it decoded test streams correctly at about 120 fps (1080p) for a fifth of one CPU core.
#define _GNU_SOURCE
#include "hwdec.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <libavcodec/bsf.h>

typedef struct { int eCodecFormat, nWidth, nHeight, nFrameRate, nFrameDuration, nAspectRatio, bIs3DStream, nCodecSpecificDataLen;
  char *pCodecSpecificData; int bSecureStreamFlag, bSecureStreamFlagLevel1, bIsFramePackage, h265ReferencePictureNum, bReOpenEngine, bIsFrameCtsTestFlag; } VideoStreamInfo;
typedef struct { int bScaleDownEn, bRotationEn, bSecOutputEn, nHorizonScaleDownRatio, nVerticalScaleDownRatio, nSDWidth, nSDHeight, bAnySizeSD,
  nSecHorizonScaleDownRatio, nSecVerticalScaleDownRatio, nRotateDegree, bThumbnailMode, eOutputPixelFormat, eSecOutputPixelFormat, bNoBFrames,
  bDisable3D, bSupportMaf, bDispErrorFrame, nVbvBufferSize, nFrameBufferNum, bSecureosEn, bGpuBufValid, nAlignStride, bIsSoftDecoderFlag,
  bVirMallocSbm, bSupportPallocBufBeforeDecode, nDeInterlaceHoldingFrameBufferNum, nDisplayHoldingFrameBufferNum, nRotateHoldingFrameBufferNum,
  nDecodeSmoothFrameBufferNum, bIsTvStream, nLbcLossyComMod; unsigned bIsLossy, bRcEn;
  void *memops; int eCtlAfbcMode, eCtlIptvMode; void *veOpsS; void *pVeOpsSelf; int bConvertVp910bitTo8bit; unsigned nVeFreq;
  int bCalledByOmxFlag, bSetProcInfoEnable, nSetProcInfoFreq, nChannelNum, nSupportMaxWidth, nSupportMaxHeight, commonConfigFlag, bATMFlag; } VConfig;
_Static_assert(__builtin_offsetof(VConfig, memops) == 136, "CedarC 1.3 VConfig: memops at 136");
_Static_assert(__builtin_offsetof(VConfig, veOpsS) == 152, "CedarC 1.3 VConfig: veOpsS at 152");
_Static_assert(__builtin_offsetof(VConfig, bSetProcInfoEnable) == 180, "CedarC 1.3 VConfig: proc info at 180");
typedef struct { char *pData; int nLength; int64_t nPts, nPcr; int bIsFirstPart, bIsLastPart, nID, nStreamIndex, bValid; unsigned bVideoInfoFlag; void *pVideoInfo; } VideoStreamDataInfo;
typedef struct { int nID, nStreamIndex, ePixelFormat, nWidth, nHeight, nLineStride, nTopOffset, nLeftOffset, nBottomOffset, nRightOffset,
  nFrameRate, nAspectRatio, bIsProgressive, bTopFieldFirst, bRepeatTopField; int64_t nPts, nPcr; char *pData0, *pData1, *pData2, *pData3; } VideoPicture;
typedef struct { int (*open)(void); int (*open2)(void *, void *); void (*close)(void); int (*total_size)(void); void *(*palloc)(int, void *, void *);
  void *(*palloc_no_cache)(int, void *, void *); void (*pfree)(void *, void *, void *); void (*flush_cache)(void *, int); } ScMemOpsS;
enum { FMT_H264 = 0x115, FMT_H265 = 0x116, PIX_YV12 = 4, R_NOFB = 4, R_NOBS = 5, R_RESCHANGE = 6 };

static void (*AddVDPlugin)(void);
static void *(*CreateVideoDecoder)(void);
static void (*DestroyVideoDecoder)(void *);
static int (*InitializeVideoDecoder)(void *, VideoStreamInfo *, VConfig *);
static int (*DecodeVideoStream)(void *, int, int, int, int64_t);
static int (*RequestVideoStreamBuffer)(void *, int, char **, int *, char **, int *, int);
static int (*SubmitVideoStreamData)(void *, VideoStreamDataInfo *, int);
static VideoPicture *(*RequestPicture)(void *, int);
static int (*ReturnPicture)(void *, VideoPicture *);
static ScMemOpsS *(*MemAdapterGetOpsS)(void);
static ScMemOpsS *mem;

struct HwDec {
  void *dec; AVBSFContext *bsf; AVRational tb; AVPacket *tmp;
  int failed, in, out;
};
static SDL_atomic_t busy;   // the video engine is one: one decoder at a time

int hwdec_available(void) {
  static int state = -1;
  if (state >= 0) return state;
  state = 0;
  const char *libs[] = { "libcdc_base.so", "libVE.so", "libMemAdapter.so", "libvideoengine.so", "libvdecoder.so" };
  for (int i = 0; i < 5; i++) if (!dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL)) { fprintf(stderr, "hwdec: %s not available\n", libs[i]); return 0; }
  #define SYM(n) do { *(void **)&n = dlsym(RTLD_DEFAULT, #n); if (!n) { fprintf(stderr, "hwdec: no %s\n", #n); return 0; } } while (0)
  SYM(AddVDPlugin); SYM(CreateVideoDecoder); SYM(DestroyVideoDecoder); SYM(InitializeVideoDecoder); SYM(DecodeVideoStream);
  SYM(RequestVideoStreamBuffer); SYM(SubmitVideoStreamData); SYM(RequestPicture); SYM(ReturnPicture); SYM(MemAdapterGetOpsS);
  #undef SYM
  AddVDPlugin();
  if (!(mem = MemAdapterGetOpsS()) || mem->open() < 0) { fprintf(stderr, "hwdec: no memory adapter\n"); return 0; }
  state = 1;
  return 1;
}

HwDec *hwdec_open(const AVCodecParameters *par, AVRational time_base, double fps) {
  int h265 = par->codec_id == AV_CODEC_ID_HEVC;
  if (par->codec_id != AV_CODEC_ID_H264 && !h265) return NULL;
  if (par->format != AV_PIX_FMT_NONE && par->format != AV_PIX_FMT_YUV420P && par->format != AV_PIX_FMT_YUVJ420P) return NULL;   // 10-bit, 4:2:2
  if (par->width > 4096 || par->height > 2304) return NULL;
  if (!hwdec_available()) return NULL;
  // The previous channel's decoder may still be closing: wait a moment for the engine.
  for (int i = 0; !SDL_AtomicCAS(&busy, 0, 1); i++) { if (i > 40) { fprintf(stderr, "hwdec: engine busy\n"); return NULL; } SDL_Delay(20); }
  HwDec *d = calloc(1, sizeof *d);
  d->tb = time_base; d->tmp = av_packet_alloc();
  if (par->extradata_size && par->extradata[0] == 1) {   // avcC / hvcC (mp4, flv): the engine wants Annex B
    if (av_bsf_alloc(av_bsf_get_by_name(h265 ? "hevc_mp4toannexb" : "h264_mp4toannexb"), &d->bsf) < 0) goto fail;
    avcodec_parameters_copy(d->bsf->par_in, par); d->bsf->time_base_in = time_base;
    if (av_bsf_init(d->bsf) < 0) goto fail;
  }
  if (!(d->dec = CreateVideoDecoder())) goto fail;
  static union { VideoStreamInfo v; char pad[1024]; } info; static union { VConfig v; char pad[4096]; } conf;   // padded: room for fields we don't know
  memset(&info, 0, sizeof info); memset(&conf, 0, sizeof conf);
  if (fps <= 0) fps = 25;
  info.v.eCodecFormat = h265 ? FMT_H265 : FMT_H264; info.v.nWidth = par->width; info.v.nHeight = par->height;
  info.v.nFrameRate = (int)(fps * 1000); info.v.nFrameDuration = (int)(1e6 / fps); info.v.bIsFramePackage = 1;
  if (!d->bsf && par->extradata_size) { info.v.pCodecSpecificData = (char *)par->extradata; info.v.nCodecSpecificDataLen = par->extradata_size; }
  conf.v.eOutputPixelFormat = PIX_YV12; conf.v.nDeInterlaceHoldingFrameBufferNum = 2; conf.v.nDisplayHoldingFrameBufferNum = 3;
  conf.v.nDecodeSmoothFrameBufferNum = 3; conf.v.memops = mem;
  if (InitializeVideoDecoder(d->dec, &info.v, &conf.v) != 0) { fprintf(stderr, "hwdec: initialize failed\n"); goto fail; }
  return d;
fail:
  hwdec_close(d);
  return NULL;
}

void hwdec_close(HwDec *d) {
  if (!d) return;
  if (d->dec) DestroyVideoDecoder(d->dec);
  av_bsf_free(&d->bsf); av_packet_free(&d->tmp);
  free(d);
  SDL_AtomicSet(&busy, 0);
}

static int submit(HwDec *d, const AVPacket *p) {
  char *b0, *b1; int l0, l1;
  if (RequestVideoStreamBuffer(d->dec, p->size, &b0, &l0, &b1, &l1, 0) != 0 || l0 + l1 < p->size) return AVERROR(EAGAIN);
  if (p->size <= l0) memcpy(b0, p->data, p->size);
  else { memcpy(b0, p->data, l0); memcpy(b1, p->data + l0, p->size - l0); }
  VideoStreamDataInfo di; memset(&di, 0, sizeof di);
  di.pData = b0; di.nLength = p->size; di.bIsFirstPart = di.bIsLastPart = 1; di.bValid = 1;
  di.nPts = p->pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(p->pts, d->tb, (AVRational){1, 1000000});
  if (SubmitVideoStreamData(d->dec, &di, 0) != 0) { d->failed = 1; return AVERROR_EXTERNAL; }
  d->in++;
  return 0;
}

int hwdec_send(HwDec *d, const AVPacket *pkt) {
  if (d->failed) return AVERROR_EXTERNAL;
  if (!d->bsf) return submit(d, pkt);
  // through the Annex B filter (one packet in, one out)
  if (!d->tmp->size) {
    if (av_bsf_send_packet(d->bsf, (AVPacket *)pkt) < 0) return 0;   // a packet it can't use: skip it
    if (av_bsf_receive_packet(d->bsf, d->tmp) < 0) return 0;
  }
  int r = submit(d, d->tmp);
  if (r != AVERROR(EAGAIN)) av_packet_unref(d->tmp);
  return r;
}

// YV12 from the engine (Y, then V, then U; chroma rows half the luma stride) into a fresh yuv420p frame, cropped.
static AVFrame *copy_picture(VideoPicture *p) {
  int bw = p->nWidth, bh = p->nHeight, stride = p->nLineStride > 0 ? p->nLineStride : bw;
  int x0 = p->nLeftOffset, y0 = p->nTopOffset, x1 = p->nRightOffset, y1 = p->nBottomOffset;
  if (x1 <= x0 || x1 > bw) { x0 = 0; x1 = bw; }
  if (y1 <= y0 || y1 > bh) { y0 = 0; y1 = bh; }
  int w = (x1 - x0) & ~1, h = (y1 - y0) & ~1;
  mem->flush_cache(p->pData0, stride * bh * 3 / 2);
  AVFrame *f = av_frame_alloc();
  f->format = AV_PIX_FMT_YUV420P; f->width = w; f->height = h;
  if (av_frame_get_buffer(f, 32) < 0) { av_frame_free(&f); return NULL; }
  const uint8_t *Y = (const uint8_t *)p->pData0, *V = Y + stride * bh, *U = V + (stride / 2) * (bh / 2);
  for (int y = 0; y < h; y++) memcpy(f->data[0] + y * f->linesize[0], Y + (y0 + y) * stride + x0, w);
  for (int y = 0; y < h / 2; y++) {
    memcpy(f->data[1] + y * f->linesize[1], U + (y0 / 2 + y) * (stride / 2) + x0 / 2, w / 2);
    memcpy(f->data[2] + y * f->linesize[2], V + (y0 / 2 + y) * (stride / 2) + x0 / 2, w / 2);
  }
  f->pts = p->nPts;
  f->sample_aspect_ratio = (AVRational){1, 1};
  return f;
}

AVFrame *hwdec_receive(HwDec *d, int *err) {
  *err = 0;
  // nothing out after a good number of packets in: this stream isn't one the engine can do
  if (d->in > 120 && d->out == 0) d->failed = 1;
  if (d->failed) { *err = 1; return NULL; }
  for (int tries = 0; tries < 4; tries++) {
    VideoPicture *p = RequestPicture(d->dec, 0);
    if (p) { AVFrame *f = copy_picture(p); ReturnPicture(d->dec, p); d->out++; return f; }
    int r = DecodeVideoStream(d->dec, 0, 0, 0, 0);
    if (r < 0 || r == R_RESCHANGE) { d->failed = 1; *err = 1; return NULL; }
    if (r == R_NOBS) return NULL;
  }
  return NULL;
}
