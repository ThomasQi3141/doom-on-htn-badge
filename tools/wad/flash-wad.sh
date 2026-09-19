#!/usr/bin/env bash
# Write a WAD image into the badge's 'wad' partition (0xB0000, 3,473,408 bytes).
#
#   tools/wad/flash-wad.sh badge.wad [/dev/cu.usbmodemXXXX]
set -euo pipefail

PY="${PY:-/private/tmp/claude-501/-Users-tq-Documents-GitHub-doom-on-htn-badge/71c78d64-9243-4c36-a33e-68fb1d0236ee/scratchpad/esp-venv/bin/python}"
WAD="${1:?usage: flash-wad.sh <image.wad> [port]}"
PORT="${2:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

WAD_OFFSET=0xB0000
WAD_MAX=$((0x350000))

size=$(stat -f%z "$WAD")
if [ "$size" -gt "$WAD_MAX" ]; then
  echo "$WAD is $size bytes; the wad partition holds $WAD_MAX."
  echo "The raw shareware IWAD (4,196,020 bytes) is larger than the whole 4 MB"
  echo "flash. Build an audio-free image instead, which does fit:"
  echo "  tools/wad/mkwad.py --no-audio DOOM1.WAD badge.wad"
  exit 1
fi

echo "Writing $WAD ($size bytes) to $WAD_OFFSET on $PORT"
"$PY" -m esptool --port "$PORT" --baud 921600 write-flash "$WAD_OFFSET" "$WAD"
