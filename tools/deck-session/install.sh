#!/bin/sh
# Installs VidyaGod as an entry of a Steam Deck's session picker (the hand-installed deck-picker: /etc/deck-boot-sessions,
# /usr/local/bin/deck-switch-session). Run as root, from this folder. Safe to run again: the entry is added once.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
install -m 755 "$HERE/vidyagod-session" /usr/local/bin/vidyagod-session
install -m 644 "$HERE/vidyagod.desktop" /usr/local/share/wayland-sessions/vidyagod.desktop
install -D -m 644 "$REPO/resources/icons/org.vidyagod.VidyaGod_512x512.png" /etc/deck-picker/icons/vidyagod.png
if ! awk '!/^#/ && $1=="vidyagod" {f=1} END {exit !f}' /etc/deck-boot-sessions; then
    printf 'vidyagod   vidyagod                                                 03        VidyaGod (Game Library)\n' >> /etc/deck-boot-sessions
fi
echo "VidyaGod is in the session picker."
