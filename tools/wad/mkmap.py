#!/usr/bin/env python3
"""Generate a minimal playable Doom map: one square room, one enemy.

The badge cannot hold a real Doom level -- E1M1 needs about 145,940 bytes of
RAM against roughly 67,332 available. This map costs on the order of 1,100,
so it fits with room to spare while still exercising the whole engine: wall,
floor and ceiling rendering, movement, collision, an enemy's AI and shooting.

A single convex room needs no BSP tree. Doom's R_RenderBSPNode treats a node
number of -1 as "subsector 0", which is exactly what an empty NODES lump
produces -- so no node builder is required.

  tools/wad/mkmap.py DOOM1.WAD out.wad        # replaces E1M1
"""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wadlib import Wad, strip_wad, build_wad

HALF      = 512           # room is 1024x1024 map units
FLOOR_H   = 0
CEIL_H    = 128
LIGHT     = 192

WALL_TEX  = "STARTAN3"
FLOOR_TEX = "FLOOR4_8"
CEIL_TEX  = "CEIL3_5"

PLAYER1   = 1
ZOMBIEMAN = 3004


def n8(s):
    return s.encode("ascii")[:8].ljust(8, b"\0")


def bam(dx, dy):
    """Doom's binary angle: the top 16 bits of a full turn."""
    a = int(round(math.atan2(dy, dx) / (2 * math.pi) * 65536)) & 0xFFFF
    return a - 0x10000 if a >= 0x8000 else a


def build_room():
    # Wound so the sector lies on each line's right (front) side, which is
    # what a one-sided wall requires.
    verts = [(-HALF, -HALF), (-HALF, HALF), (HALF, HALF), (HALF, -HALF)]
    edges = [(0, 1), (1, 2), (2, 3), (3, 0)]

    VERTEXES = b"".join(struct.pack("<hh", x, y) for x, y in verts)

    # x, y, angle, type, flags (7 = present on all three skill groups)
    THINGS = (struct.pack("<hhhhh", 0, -256, 90, PLAYER1, 7) +
              struct.pack("<hhhhh", 0,  256, 270, ZOMBIEMAN, 7))

    # v1, v2, flags, special, tag, right sidedef, left sidedef (0xFFFF = none)
    LINEDEFS = b"".join(
        struct.pack("<HHhhhHH", a, b, 1, 0, 0, i, 0xFFFF)
        for i, (a, b) in enumerate(edges))

    # xoff, yoff, upper, lower, middle, sector
    SIDEDEFS = b"".join(
        struct.pack("<hh8s8s8sh", 0, 0, n8("-"), n8("-"), n8(WALL_TEX), 0)
        for _ in edges)

    # floor, ceiling, floorpic, ceilpic, light, special, tag
    SECTORS = struct.pack("<hh8s8shhh", FLOOR_H, CEIL_H,
                          n8(FLOOR_TEX), n8(CEIL_TEX), LIGHT, 0, 0)

    # v1, v2, angle, linedef, side, offset -- one seg per wall
    SEGS = b""
    for i, (a, b) in enumerate(edges):
        dx = verts[b][0] - verts[a][0]
        dy = verts[b][1] - verts[a][1]
        SEGS += struct.pack("<HHhHhh", a, b, bam(dx, dy), i, 0, 0)

    SSECTORS = struct.pack("<HH", len(edges), 0)   # one subsector, all the segs
    NODES = b""                                    # empty: R_RenderBSPNode(-1)

    # All sectors see all sectors. One sector, so one zero byte.
    REJECT = b"\0"

    # Every block shares one list naming all four lines. Listing extra lines in
    # a block is conservative but correct -- the blockmap only narrows the set
    # of candidates collision detection then checks properly.
    cols = rows = 10
    origin = -640
    header = struct.pack("<hhhh", origin, origin, cols, rows)
    first_list = 4 + cols * rows            # in shorts
    offsets = struct.pack("<%dH" % (cols * rows), *([first_list] * (cols * rows)))
    blocklist = struct.pack("<%dh" % (len(edges) + 2),
                            0, *range(len(edges)), -1)
    BLOCKMAP = header + offsets + blocklist

    # Doom requires this exact order after the map marker.
    return [("THINGS", THINGS), ("LINEDEFS", LINEDEFS), ("SIDEDEFS", SIDEDEFS),
            ("VERTEXES", VERTEXES), ("SEGS", SEGS), ("SSECTORS", SSECTORS),
            ("NODES", NODES), ("SECTORS", SECTORS), ("REJECT", REJECT),
            ("BLOCKMAP", BLOCKMAP)]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    target = sys.argv[3] if len(sys.argv) > 3 else "E1M1"

    wad = Wad(src)
    names = [n.upper() for n, _, _ in wad.lumps]
    for t, kind in ((WALL_TEX, "wall texture"),
                    (FLOOR_TEX, "flat"), (CEIL_TEX, "flat")):
        if t.upper() not in names and kind == "flat":
            print(f"  warning: {kind} {t} not found in {src}")

    room = build_room()
    room_bytes = sum(len(d) for _, d in room)

    lumps = strip_wad(wad)
    out, replaced, i = [], False, 0
    while i < len(lumps):
        name, data = lumps[i]
        if name.upper() == target.upper():
            out.append((name, b""))            # the map marker itself
            out.extend(room)
            i += 11                            # skip the original ten lumps
            replaced = True
            continue
        out.append((name, data))
        i += 1

    if not replaced:
        print(f"{target} not found in {src}")
        return 1

    img = build_wad(out)
    with open(dst, "wb") as f:
        f.write(img)

    print(f"{target} replaced with a {HALF*2}x{HALF*2} room "
          f"({room_bytes} bytes of map data)")
    for n, d in room:
        print(f"    {n:<10}{len(d):>6}")
    print(f"\n{dst}: {len(img):,} bytes, {len(out)} lumps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
