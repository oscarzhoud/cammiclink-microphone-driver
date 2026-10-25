#!/bin/bash
# Install the locally built driver into the system HAL plug-in directory.
# Replaces only CamMicLinkMic.driver; leaves every other audio component alone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/CamMicLinkMic.driver"
DEST="/Library/Audio/Plug-Ins/HAL/CamMicLinkMic.driver"

if [ ! -d "$SRC" ]; then
  echo "Driver not built yet. Run ./build.sh first." >&2
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "Administrator rights are required. Run: sudo \"$0\"" >&2
  exit 1
fi

/usr/bin/install -d -o root -g wheel -m 755 /Library/Audio/Plug-Ins/HAL
rm -rf "$DEST"
ditto "$SRC" "$DEST"
chown -R root:wheel "$DEST"
chmod -R go-w "$DEST"

# Ask coreaudiod to reload the plug-in. Audio drops for a moment.
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null ||
  killall coreaudiod 2>/dev/null || true

echo "Installed: $DEST"
echo 'Restart audio apps and choose "CamMicLink Microphone" as their input.'
