#!/bin/sh
# Builds dist/Tuner.pak.zip with the pak's files at the top of the zip (the layout Pak Store expects).
# For a manual install, unzip it into a new folder: SD card Tools/tg5040/Tuner.pak/.
set -e
cd "$(dirname "$0")"
# Here (not in GitHub's release workflow, which packs the binary kept in git): build it and put it in the pak first
if [ -z "$CI" ]; then build/build-app.sh >/dev/null; cp build/out/tuner Tuner.pak/tuner; fi
mkdir -p dist
rm -f dist/Tuner.pak.zip
rm -rf Tuner.pak/fonts && cp -r ui/dot/fonts Tuner.pak/fonts   # Dot's fonts (ui/dot is a submodule)
(cd Tuner.pak && zip -q -X -r ../dist/Tuner.pak.zip tuner launch.sh fonts playlists.example.txt LICENSE-cJSON LICENSE-ffmpeg-LGPL)
unzip -l dist/Tuner.pak.zip
