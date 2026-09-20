# Prebuilt Doom demo

Flash onto any 2026 Hacker Badge with:

    tools/flash-badge.sh [/dev/cu.usbmodemXXXX]

**Back the badge up first**, once per badge:

    tools/flash/badge-safety-check.sh

That saves the full 4 MB stock image, which holds the badge's provisioned
identity and contacts. There is no other way to get it back. Restore with:

    esptool --port <port> --baud 921600 write-flash 0 backup/stock-<mac>-<stamp>.bin

## What is here

| file | offset | what |
|---|---|---|
| `bootloader.bin` | `0x0` | ESP-IDF second-stage bootloader |
| `partition-table.bin` | `0x8000` | 1 MB app + 3,080,192 B wad |
| `badge_doom.bin` | `0x10000` | the Doom engine and badge platform layer |
| `doom-arena.wad` | `0x110000` | game data plus the purpose-built arena |

`doom-arena.wad` is built from the shareware `DOOM1.WAD` and carries id
Software's assets, so it is not committed. Regenerate it with:

    tools/wad/mkarena.py DOOM1.WAD dist/doom-arena.wad

## The build it came from

Doom running at about 20 fps on a 160 MHz RISC-V core with 400 KB of RAM,
rendering 320x200 at 8bpp, scaled 1.2x to the 320x240 panel for correct
aspect. The WAD is memory-mapped from flash and never read into RAM.

`lua-app/` holds the original Lua badge app from before the port; kept for
reference only.
