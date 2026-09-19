#!/usr/bin/env bash
# Rebuild the Doom demo from source, end to end.
#
#   tools/build-demo.sh /path/to/DOOM1.WAD
#
# Produces dist/{bootloader,partition-table,badge_doom}.bin and
# dist/doom-arena.wad. Flash with tools/flash-badge.sh.

set -euo pipefail
cd "$(dirname "$0")/.."

IWAD="${1:-}"
[[ -f "$IWAD" ]] || { echo "usage: tools/build-demo.sh /path/to/DOOM1.WAD"; exit 1; }
[[ -d "${IDF_PATH:-}" ]] || { echo "source \$IDF_PATH/export.sh first"; exit 1; }

mkdir -p dist

echo "== building the arena =="
python3 tools/wad/mkarena.py "$IWAD" dist/doom-arena.wad

echo
echo "== building firmware =="
( cd firmware/doom && idf.py build )

cp firmware/doom/build/bootloader/bootloader.bin dist/
cp firmware/doom/build/partition_table/partition-table.bin dist/
cp firmware/doom/build/badge_doom.bin dist/

shasum -a 256 dist/bootloader.bin dist/partition-table.bin \
              dist/badge_doom.bin dist/doom-arena.wad > dist/SHA256SUMS

echo
echo "== dist =="
ls -l dist/*.bin dist/*.wad | awk '{printf "  %-34s %9d\n", $NF, $5}'
echo
echo "Flash with: tools/flash-badge.sh"
