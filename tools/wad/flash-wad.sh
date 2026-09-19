#!/usr/bin/env bash
# Write a WAD image into the badge's 'wad' partition (0x110000, 3,080,192 bytes).
#
#   tools/wad/flash-wad.sh badge.wad [/dev/cu.usbmodemXXXX]
set -euo pipefail

PY="${PY:-/private/tmp/claude-501/-Users-tq-Documents-GitHub-doom-on-htn-badge/71c78d64-9243-4c36-a33e-68fb1d0236ee/scratchpad/esp-venv/bin/python}"
WAD="${1:?usage: flash-wad.sh <image.wad> [port]}"
PORT="${2:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

WAD_OFFSET=0x110000
WAD_MAX=$((0x2F0000))

size=$(stat -f%z "$WAD")
if [ "$size" -gt "$WAD_MAX" ]; then
  echo "$WAD is $size bytes; the wad partition holds $WAD_MAX."
  echo "The full shareware IWAD (4,196,020 bytes) does not fit -- and is larger"
  echo "than the whole 4 MB flash. Use tools/wad/mkwad.py to build a subset."
  exit 1
fi

echo "Writing $WAD ($size bytes) to $WAD_OFFSET on $PORT"
"$PY" -m esptool --port "$PORT" --baud 921600 write-flash "$WAD_OFFSET" "$WAD"
