#!/bin/sh
# Builds a minimal static ffmpeg 6.1 for IPTV in the ARM64 container: HLS/MPEG-TS/MP4 over HTTP(S) (OpenSSL 1.1,
# the Brick's), H.264/H.265/MPEG-2 video and AAC/MP3/MP2/AC-3 audio. Static, so it never clashes with the Brick's
# own ffmpeg 6.1 (/usr/lib/libav*.so.60, built without HLS or HTTPS). Output: build/out/ffmpeg/{include,lib}.
set -e
cd "$(dirname "$0")/.."
docker build -q -t nextui-iptv-build build >/dev/null
mkdir -p build/out
docker run --rm -v "$PWD:/w" nextui-iptv-build sh -c '
set -e
cd /tmp && tar xJf /w/deps/ffmpeg-6.1.2.tar.xz && cd ffmpeg-6.1.2
./configure --prefix=/w/build/out/ffmpeg --enable-static --disable-shared --enable-pic \
  --disable-programs --disable-doc --disable-debug --disable-everything --disable-autodetect \
  --enable-pthreads --enable-openssl --enable-swresample --enable-swscale \
  --enable-protocol=http,https,tls,tcp,file,crypto,hls \
  --enable-demuxer=hls,mpegts,mov,aac,mp3,flv,mpegps \
  --enable-decoder=h264,hevc,mpeg2video,mpeg4,aac,aac_latm,mp3,mp2,ac3,eac3 \
  --enable-parser=h264,hevc,mpegvideo,mpeg4video,aac,aac_latm,mpegaudio,ac3 \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc >/dev/null
make -j6 >/dev/null 2>&1 && make install >/dev/null
ls /w/build/out/ffmpeg/lib
'
