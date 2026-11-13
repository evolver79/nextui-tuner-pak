#!/bin/sh
# Builds dist/Tuner.pak.zip with the pak's files at the top of the zip (the layout Pak Store expects).
# For a manual install, unzip it into a new folder: SD card Tools/tg5040/Tuner.pak/.
set -e
cd "$(dirname "$0")"
mkdir -p dist
rm -f dist/Tuner.pak.zip
(cd Tuner.pak && zip -q -X ../dist/Tuner.pak.zip tuner launch.sh bedstead.otf playlists.example.txt LICENSE-cJSON LICENSE-ffmpeg-LGPL)
unzip -l dist/Tuner.pak.zip
