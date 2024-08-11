#!/bin/bash
WINEPREFIX="/home/lorenzo-zurini/RUNTIME/" \
GAMEID="aoe2" \
PROTONPATH="/usr/share/steam/compatibilitytools.d/proton-ge-custom/" \
gamescope -f -b --framerate-limit 200 --force-grab-cursor --adaptive-sync \
umu-run "C:/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/age2_x1.exe" game=age2_x2
#WINEDLLOVERRIDES="winmm=n,b"
