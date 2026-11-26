#!/bin/sh
# Builds a minimal static ffmpeg 6.1 for IPTV in the ARM64 container: HLS/MPEG-TS/MP4 over HTTP(S) (OpenSSL 1.1,
# the Brick's), H.264/H.265/MPEG-2 video and AAC/MP3/MP2/AC-3 audio. Static, so it never clashes with the Brick's
# own ffmpeg 6.1 (/usr/lib/libav*.so.60, built without HLS or HTTPS). Also packed AC-3/E-AC-3 audio in HLS, closed
# captions (ccaption) and DVB subtitles (dvbsub). Output: build/out/ffmpeg/{include,lib}.
set -e
cd "$(dirname "$0")/.."
docker build -q -t nextui-iptv-build build >/dev/null
mkdir -p build/out
docker run --rm -v "$PWD:/w" nextui-iptv-build sh -c '
set -e
cd /tmp && tar xJf /w/deps/ffmpeg-6.1.2.tar.xz && cd ffmpeg-6.1.2
git apply /w/build/ffmpeg-hls-interleave.patch   # HLS: playlists interleaved on their own continuous timelines (ad splices)
./configure --prefix=/w/build/out/ffmpeg --enable-static --disable-shared --enable-pic \
  --disable-programs --disable-doc --disable-debug --disable-everything --disable-autodetect \
  --enable-pthreads --enable-openssl --enable-swresample --enable-swscale \
  --enable-protocol=http,https,tls,tcp,file,crypto,hls \
  --enable-demuxer=hls,mpegts,mov,aac,mp3,flv,mpegps,ac3,eac3 \
  --enable-decoder=h264,hevc,mpeg2video,mpeg4,aac,aac_latm,mp3,mp2,ac3,eac3,ccaption,dvbsub \
  --enable-parser=h264,hevc,mpegvideo,mpeg4video,aac,aac_latm,mpegaudio,ac3,dvbsub \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc >/dev/null
make -j6 >/dev/null 2>&1 && make install >/dev/null
ls /w/build/out/ffmpeg/lib
'
