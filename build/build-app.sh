#!/bin/sh
# Builds Tuner (build/out/tuner) against the static ffmpeg from build-ffmpeg.sh, in the ARM64 container.
set -e
cd "$(dirname "$0")/.."
[ -d build/out/ffmpeg ] || build/build-ffmpeg.sh
docker build -q -t nextui-iptv-build build >/dev/null
docker run --rm -v "$PWD:/w" -w /w nextui-iptv-build sh -c '
F=build/out/ffmpeg
gcc -O2 -Wall -Wno-format-truncation -o build/out/tuner src/app.c src/tv.c src/tt.c src/themes.c src/logos.c src/channels.c src/sources.c src/net.c src/store.c src/vendor/cJSON.c \
  -I$F/include $(pkg-config --cflags sdl2) \
  $F/lib/libavformat.a $F/lib/libavcodec.a $F/lib/libswscale.a $F/lib/libswresample.a $F/lib/libavutil.a \
  $(pkg-config --libs sdl2) -lSDL2_image -lSDL2_ttf -lssl -lcrypto -lz -lpthread -lm -ldl
strip build/out/tuner
objdump -p build/out/tuner | grep NEEDED | tr -s " " | cut -d" " -f3 | tr "\n" " "; echo
'
