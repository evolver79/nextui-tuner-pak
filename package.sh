#!/bin/sh
# Builds dist/Tuner.pak.zip with the pak's files at the top of the zip (the layout Pak Store expects).
# For a manual install, unzip it into a new folder: SD card Tools/tg5040/Tuner.pak/.
set -e
cd "$(dirname "$0")"
mkdir -p dist
rm -f dist/Tuner.pak.zip
rm -rf Tuner.pak/fonts && cp -r ui/dot/fonts Tuner.pak/fonts   # Dot's fonts (ui/dot is a submodule)
(cd Tuner.pak && zip -q -X -r ../dist/Tuner.pak.zip tuner launch.sh fonts playlists.example.txt LICENSE-cJSON LICENSE-ffmpeg-LGPL)
unzip -l dist/Tuner.pak.zip
