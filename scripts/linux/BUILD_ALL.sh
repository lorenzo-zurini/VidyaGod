#!/bin/bash
STEAM_COMPAT_DATA_PATH="/home/lorenzo-zurini/DEFPREFIX" proton wineboot

mkdir -p "/home/lorenzo-zurini/TEMP/[0]"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/DEFPREFIX" "/home/lorenzo-zurini/TEMP/[0]"

mkdir -p "/home/lorenzo-zurini/TEMP/[1]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Age of Kings" "/home/lorenzo-zurini/TEMP/[1]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[2]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Age of Kings - Patch 2.0a" "/home/lorenzo-zurini/TEMP/[2]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[3]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Age of Kings - No-CD Patch by RADiATiON" "/home/lorenzo-zurini/TEMP/[3]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[4]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Conquerors" "/home/lorenzo-zurini/TEMP/[4]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[5]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Conquerors - Patch 1.0c" "/home/lorenzo-zurini/TEMP/[5]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[6]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Conquerors - Patch 1.0e (Unofficial)" "/home/lorenzo-zurini/TEMP/[6]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[7]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - The Conquerors - UserPatch v1.5 Build 6268" "/home/lorenzo-zurini/TEMP/[7]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[8]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/Age of Empires II - Forgotten Empires v2.2" "/home/lorenzo-zurini/TEMP/[8]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II"

mkdir -p "/home/lorenzo-zurini/TEMP/[9]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/Sound/midi"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/MIDI Soundtrack" "/home/lorenzo-zurini/TEMP/[9]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/Sound/midi"

mkdir -p "/home/lorenzo-zurini/TEMP/[10]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/FLAC Soundtrack"
bindfs --no-allow-other -o ro "/home/lorenzo-zurini/Vidya/[749] Age of Empires II/FLAC Soundtrack" "/home/lorenzo-zurini/TEMP/[10]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/FLAC Soundtrack"

#mkdir -p "/home/lorenzo-zurini/TEMP/[10]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/"
#bindfs --no-allow-other -o ro "/home/lorenzo-zurini/ogg-winmm_rev3" "/home/lorenzo-zurini/TEMP/[10]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1"

#mkdir -p "/home/lorenzo-zurini/TEMP/[11]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/Music"
#bindfs --no-allow-other -o ro "/home/lorenzo-zurini/OGG Soundtrack" "/home/lorenzo-zurini/TEMP/[11]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/Music"

#mkdir -p "/home/lorenzo-zurini/TEMP/[12]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1"
#ln "/home/lorenzo-zurini/dgVoodoo2_8/MS/x86/DDraw.dll" "/home/lorenzo-zurini/TEMP/[12]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/DDraw.dll"
#ln "/home/lorenzo-zurini/dgVoodoo2_8/MS/x86/D3D8.dll" "/home/lorenzo-zurini/TEMP/[12]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/D3D8.dll"
#ln "/home/lorenzo-zurini/dgVoodoo2_8/MS/x86/D3D9.dll" "/home/lorenzo-zurini/TEMP/[12]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/D3D9.dll"
#ln "/home/lorenzo-zurini/dgVoodoo2_8/MS/x86/D3DImm.dll" "/home/lorenzo-zurini/TEMP/[12]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/D3DImm.dll"
#ln "/home/lorenzo-zurini/dgVoodoo2_8/dgVoodoo.conf" "/home/lorenzo-zurini/TEMP/[12]/pfx/drive_c/Program Files (x86)/Microsoft Games/Age of Empires II/age2_x1/dgVoodoo.conf"

mkdir -p "/home/lorenzo-zurini/RUNTIME/"
mkdir -p "/home/lorenzo-zurini/CHANGES/"

unionfs -o cow \
"/home/lorenzo-zurini/CHANGES/"=RW:\
"/home/lorenzo-zurini/TEMP/[10]/"=RO:\
"/home/lorenzo-zurini/TEMP/[9]/"=RO:\
"/home/lorenzo-zurini/TEMP/[8]/"=RO:\
"/home/lorenzo-zurini/TEMP/[7]/"=RO:\
"/home/lorenzo-zurini/TEMP/[6]/"=RO:\
"/home/lorenzo-zurini/TEMP/[5]/"=RO:\
"/home/lorenzo-zurini/TEMP/[4]/"=RO:\
"/home/lorenzo-zurini/TEMP/[3]/"=RO:\
"/home/lorenzo-zurini/TEMP/[2]/"=RO:\
"/home/lorenzo-zurini/TEMP/[1]/"=RO:\
"/home/lorenzo-zurini/TEMP/[0]/"=RO \
"/home/lorenzo-zurini/RUNTIME/"

exit

#"/home/lorenzo-zurini/TEMP/[12]/"=RO:\
#"/home/lorenzo-zurini/TEMP/[11]/"=RO:\
#"/home/lorenzo-zurini/TEMP/[10]/"=RO:\
