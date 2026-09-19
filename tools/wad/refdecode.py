#!/usr/bin/env python3
"""Reference decode of a lump, for checking the device against the host.

The badge prints an FNV-1a hash of its 320x200 framebuffer after decoding a
lump. This prints what that hash should be. Matching values mean the flash
mapping, the lump lookup and the column decoder are all correct -- verifiable
without looking at the screen.

  tools/wad/refdecode.py badge.wad TITLEPIC
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wadlib import Wad, decode_patch, fnv1a

W, H = 320, 200


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    names = sys.argv[2:] or ["TITLEPIC"]

    wad = Wad(path)
    for name in names:
        raw = wad.find(name)
        if raw is None:
            print(f"{name}: not in {path}")
            continue
        fb, w, h, lo, to = decode_patch(raw, W, H)
        used = len(set(fb))
        print(f"{name}: {len(raw):,} bytes, {w}x{h}, offset ({lo},{to})")
        print(f"  {used} distinct palette indices used")
        print(f"  framebuffer FNV1a = 0x{fnv1a(fb):08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
