#!/usr/bin/env python3
"""Break a WAD down by category, to decide what has to be cut or recompressed.

The badge has 4 MB of flash total and no audio hardware of any kind, so the
question is how much of the game survives dropping every sound and every piece
of music, and what remains to be compressed after that.
"""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wadlib import Wad

LEVEL_PARTS = {"THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS",
               "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"}
MAP_RE = re.compile(r"^(E\dM\d|MAP\d\d)$")


def categorize(wad):
    cats = {}
    section = None
    current_map = None

    for name, pos, size in wad.lumps:
        u = name.upper()

        # Section markers delimit sprites, flats and wall patches.
        if u in ("S_START", "SS_START"): section = "sprites"; continue
        if u in ("S_END", "SS_END"):     section = None;      continue
        if u in ("F_START", "FF_START"): section = "flats";   continue
        if u in ("F_END", "FF_END"):     section = None;      continue
        if u in ("P_START", "PP_START"): section = "patches"; continue
        if u in ("P_END", "PP_END"):     section = None;      continue

        if MAP_RE.match(u):
            current_map = u
            cat = "levels"
        elif u in LEVEL_PARTS and current_map:
            cat = "levels"
        elif u.startswith("D_"):
            cat = "music"
        elif u.startswith("DS"):
            cat = "sfx"
        elif u.startswith("DP"):
            cat = "pcspeaker"
        elif section:
            cat = section
        elif u in ("PLAYPAL", "COLORMAP", "TEXTURE1", "TEXTURE2", "PNAMES"):
            cat = "core tables"
        elif u == "ENDOOM":
            cat = "misc"
        else:
            cat = "ui graphics"

        e = cats.setdefault(cat, [0, 0])
        e[0] += size
        e[1] += 1
    return cats


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "DOOM1.WAD"
    wad = Wad(path)
    total = len(wad.data)
    cats = categorize(wad)

    print(f"{path}: {wad.ident}, {len(wad.lumps)} lumps, {total:,} bytes\n")
    print(f"{'category':<14}{'bytes':>12}{'lumps':>8}{'share':>8}")
    print("-" * 42)
    for cat, (size, n) in sorted(cats.items(), key=lambda kv: -kv[1][0]):
        print(f"{cat:<14}{size:>12,}{n:>8}{size/total*100:>7.1f}%")

    audio = sum(cats.get(c, [0, 0])[0] for c in ("music", "sfx", "pcspeaker"))
    kept = total - audio

    FLASH = 4 * 1024 * 1024
    WAD_PART = 0x2F0000

    print("\n--- the badge has no speaker, so all audio is dead weight ---")
    print(f"audio total          {audio:>12,}  ({audio/total*100:.1f}%)")
    print(f"WAD without audio    {kept:>12,}")
    print(f"wad partition        {WAD_PART:>12,}")
    print(f"still over by        {kept - WAD_PART:>12,}"
          if kept > WAD_PART else
          f"FITS with room       {WAD_PART - kept:>12,}")

    if kept > WAD_PART:
        ratio = WAD_PART / kept
        print(f"\nneeds to shrink to {ratio*100:.0f}% of its audio-free size")
    return 0


if __name__ == "__main__":
    sys.exit(main())
