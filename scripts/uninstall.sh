#!/bin/bash
# Remove CamMicLink's driver. No other audio component is touched.
set -euo pipefail
DEST="/Library/Audio/Plug-Ins/HAL/CamMicLinkMic.driver"

if [ "$(id -u)" -ne 0 ]; then
  echo "Administrator rights are required. Run: sudo \"$0\"" >&2
  exit 1
fi

if [ ! -d "$DEST" ]; then
  echo "Not installed: $DEST"
  exit 0
fi

rm -rf "$DEST"
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null ||
  killall coreaudiod 2>/dev/null || true
echo "Removed: $DEST"
