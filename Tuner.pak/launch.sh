#!/bin/sh
# Tuner – IPTV Player: free TV for NextUI. Data (settings, favourites, cache) lives in NextUI's user data folder.
cd "$(dirname "$0")"
./tuner "$USERDATA_PATH/Tuner" "$PWD" > "$LOGS_PATH/Tuner.txt" 2>&1
