#!/usr/bin/env python3
"""Pack selected lumps from an IWAD into a small WAD for the badge.

The shareware DOOM1.WAD is 4,196,020 bytes -- larger than the badge's entire
4 MB flash -- so the full game needs a recompressed format. This tool exists for
the step before that: it produces a real WAD, in the real format, small enough
to flash, so the on-device reader can be proven against genuine Doom data.

  tools/wad/mkwad.py --no-audio DOOM1.WAD badge.wad     # the whole game, minus sound
  tools/wad/mkwad.py DOOM1.WAD small.wad PLAYPAL TITLEPIC   # just these lumps
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wadlib import Wad, build_wad, strip_wad

DEFAULT_LUMPS = ["PLAYPAL", "COLORMAP", "TITLEPIC", "HELP1", "CREDIT",
                 "M_DOOM", "STBAR", "ENDOOM"]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    args = sys.argv[1:]
    no_audio = "--no-audio" in args
    if no_audio:
        args.remove("--no-audio")
    src, dst = args[0], args[1]
    names = args[2:] or DEFAULT_LUMPS

    wad = Wad(src)
    print(f"{src}: {wad.ident}, {len(wad.lumps)} lumps, {len(wad.data):,} bytes")

    if no_audio:
        picked = strip_wad(wad)
        dropped = len(wad.lumps) - len(picked)
        print(f"  dropped {dropped} audio/extra lumps the badge cannot use")
    else:
        picked, missing = [], []
        for n in names:
            data = wad.find(n)
            if data is None:
                missing.append(n)
            else:
                picked.append((n, data))
                print(f"  + {n:<10} {len(data):>9,} bytes")
        for n in missing:
            print(f"  - {n:<10} not present")

    out = build_wad(picked)
    with open(dst, "wb") as f:
        f.write(out)
    part = 0x350000
    print(f"\n{dst}: {len(out):,} bytes, {len(picked)} lumps "
          f"({len(out) / part * 100:.1f}% of the {part:,}-byte wad partition)")
    if len(out) > part:
        print(f"  TOO BIG by {len(out) - part:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
