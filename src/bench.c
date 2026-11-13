// Decode benchmark for the TrimUI Brick: opens an IPTV stream (HLS etc.), decodes video for N seconds and reports
// resolution, codec, the stream's frame rate, ms of decode time per frame and the decode speed vs real time.
// Usage: bench <url> [seconds=20]
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: bench <url> [seconds]\n"); return 1; }
  double secs = argc > 2 ? atof(argv[2]) : 20;
  avformat_network_init();
  AVFormatContext *fmt = NULL;
  AVDictionary *opt = NULL;
  av_dict_set(&opt, "rw_timeout", "8000000", 0);
  int64_t t0 = av_gettime_relative();
  if (avformat_open_input(&fmt, argv[1], NULL, &opt) < 0) { printf("RESULT open failed\n"); return 2; }
  if (avformat_find_stream_info(fmt, NULL) < 0) { printf("RESULT no stream info\n"); return 2; }
  printf("opened in %.1f s\n", (av_gettime_relative() - t0) / 1e6);
  // HLS master playlists expose every variant as a program; pick the video stream with the fewest pixels <= 720p,
  // like the player will (falls back to the smallest one there is).
  int vi = -1; int best = 1 << 30;
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    AVCodecParameters *p = fmt->streams[i]->codecpar;
    if (p->codec_type != AVMEDIA_TYPE_VIDEO) continue;
    int px = p->width * p->height;
    printf("  video stream %u: %dx%d %s\n", i, p->width, p->height, avcodec_get_name(p->codec_id));
    int score = (p->height <= 720 ? 0 : 1 << 28) + (p->height <= 720 ? -px : px);
    if (score < best) { best = score; vi = i; }
  }
  if (vi < 0) { printf("RESULT no video\n"); return 2; }
  for (unsigned i = 0; i < fmt->nb_streams; i++) fmt->streams[i]->discard = (int)i == vi ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  AVStream *st = fmt->streams[vi];
  const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *c = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(c, st->codecpar);
  c->thread_count = 4; c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  if (avcodec_open2(c, dec, NULL) < 0) { printf("RESULT decoder failed\n"); return 2; }
  double fps = av_q2d(st->avg_frame_rate); if (fps <= 0) fps = av_q2d(st->r_frame_rate);
  AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
  int frames = 0; int64_t dec_us = 0, start = av_gettime_relative();
  while (av_gettime_relative() - start < secs * 1e6) {
    if (av_read_frame(fmt, pkt) < 0) break;
    if (pkt->stream_index == vi) {
      int64_t a = av_gettime_relative();
      if (avcodec_send_packet(c, pkt) == 0) while (avcodec_receive_frame(c, fr) == 0) frames++;
      dec_us += av_gettime_relative() - a;
    }
    av_packet_unref(pkt);
  }
  double ms = frames ? dec_us / 1000.0 / frames : 0;
  printf("RESULT %dx%d %s %.2f fps: %d frames, %.1f ms/frame decode, %.2fx real time\n", c->width, c->height,
         dec->name, fps, frames, ms, ms > 0 ? (1000.0 / fps) / ms : 0);
  return 0;
}
