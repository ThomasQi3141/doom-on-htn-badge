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

# Python with esptool installed: $PY, else ~/esp/esp-venv, else whatever
# python3 is on PATH.
PY="${PY:-}"
if [[ -z "$PY" ]]; then
  if [[ -x "$HOME/esp/esp-venv/bin/python" ]]; then PY="$HOME/esp/esp-venv/bin/python"
  else PY="$(command -v python3)"; fi
fi
if ! "$PY" -m esptool version >/dev/null 2>&1; then
  echo "esptool not found for $PY"
  echo "Create a venv with:  python3 -m venv ~/esp/esp-venv && ~/esp/esp-venv/bin/pip install esptool"
  exit 1
fi
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
# 0x10000  application  (640 KB partition)
# 0xB0000  wad          (3,473,408 B partition)
"$PY" -m esptool --port "$PORT" --before default-reset --baud 921600 \
  write-flash \
  0x0      dist/bootloader.bin \
  0x8000   dist/partition-table.bin \
  0x10000  dist/badge_doom.bin \
  0xB0000  dist/doom-arena.wad

echo
echo "Done. Power-cycle the badge; it boots straight into the arena."
