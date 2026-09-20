#!/usr/bin/env bash
# Flash the prebuilt Doom demo onto a badge.
#
# Everything needed is in dist/. This does NOT back the badge up first --
# run tools/flash/badge-safety-check.sh for that, once per badge, and keep
# the result. The stock image holds that badge's provisioned identity and it
# cannot be recovered any other way.
#
#   tools/flash-badge.sh [/dev/cu.usbmodemXXXX]

set -euo pipefail
cd "$(dirname "$0")/.."

PY="${PY:-/private/tmp/claude-501/-Users-tq-Documents-GitHub-doom-on-htn-badge/71c78d64-9243-4c36-a33e-68fb1d0236ee/scratchpad/esp-venv/bin/python}"
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

if [[ -z "${PORT:-}" ]]; then
  echo "No badge found on /dev/cu.usbmodem*."
  echo "Power it off, connect a USB data cable, power it on, and retry."
  echo "If the badge is running its stock firmware, esptool may need a couple"
  echo "of attempts -- it does not honour the usual auto-reset."
  exit 1
fi

for f in dist/bootloader.bin dist/partition-table.bin dist/badge_doom.bin dist/doom-arena.wad; do
  [[ -f "$f" ]] || { echo "missing $f -- run tools/build-demo.sh"; exit 1; }
done

echo "Verifying artifacts..."
shasum -a 256 -c dist/SHA256SUMS --ignore-missing --quiet || {
  echo "  checksums do not match dist/SHA256SUMS"; exit 1; }

echo "Flashing $PORT"
# 0x0      bootloader
# 0x8000   partition table
# 0x10000  application  (896 KB partition)
# 0xF0000  wad          (3,211,264 B partition)
"$PY" -m esptool --port "$PORT" --before default-reset --baud 921600 \
  write-flash \
  0x0      dist/bootloader.bin \
  0x8000   dist/partition-table.bin \
  0x10000  dist/badge_doom.bin \
  0xF0000  dist/doom-arena.wad

echo
echo "Done. Power-cycle the badge; it boots straight into the arena."
