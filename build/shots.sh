#!/bin/sh
# Builds Tuner, then renders every page headless (SDL dummy video, software renderer) into build/out/shots/*.png, with
# the channel lists cached in build/out/shotuser. DOT_SCREEN=1280x720 renders the Smart Pro's screen.
set -e
cd "$(dirname "$0")/.."
build/build-app.sh >/dev/null
O=build/out/shots
rm -rf $O && mkdir -p $O/pak && cp -r build/out/shotuser $O/user   # a copy: the shots add favourites
docker run --rm -v "$PWD:/w" -w /w -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy -e DOT_SCREEN=${DOT_SCREEN:-1024x768} nextui-iptv-build sh -c "
cp -r ui/dot/fonts $O/pak/ && build/out/tuner $O/user $O/pak --shots $O >/dev/null 2>&1"
for f in $O/*.bmp; do ffmpeg -v error -y -i "$f" "${f%.bmp}.png" && rm "$f"; done   # ffmpeg on the Mac (brew)
rm -rf $O/user $O/pak; ls $O/*.png
