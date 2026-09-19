#!/usr/bin/env python3
"""Carve a playable fragment out of a real Doom level.

A whole level will not fit in the badge's zone heap -- E1M1 needs about
145,940 bytes against roughly 67,332 free. But a *piece* of one will, and it
keeps the original architecture, textures and thing placement, so it reads as
real Doom rather than as a test map.

Sectors are taken by flooding outward from the player start through two-sided
lines, so whatever survives is always reachable on foot. Lines that led to a
dropped sector are sealed into plain walls. Cutting geometry invalidates the
BSP, so SEGS/SSECTORS/NODES/BLOCKMAP/REJECT are rebuilt with zdbsp.

  tools/wad/cutmap.py DOOM1.WAD out.wad --sectors 30
"""
import sys, os, struct, subprocess, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wadlib import Wad, strip_wad, build_wad

ML_BLOCKING = 1
ML_TWOSIDED = 4
SEAL_TEX    = "STARTAN3"

# Bytes of badge RAM per record, measured on hardware.
COST = {"linedefs": 111, "sidedefs": 32.5, "sectors": 148,
        "segs": 40, "nodes": 56, "ssectors": 8, "vertexes": 12.8}


def texture_table(wad):
    """name -> (width, height, patchcount) for every wall texture."""
    out = {}
    for lump in ("TEXTURE1", "TEXTURE2"):
        raw = wad.find(lump)
        if not raw:
            continue
        n, = struct.unpack_from("<i", raw, 0)
        for off in struct.unpack_from(f"<{n}i", raw, 4):
            name = raw[off:off+8].rstrip(b"\0").decode("ascii", "replace").upper()
            w, h = struct.unpack_from("<hh", raw, off+12)
            pc, = struct.unpack_from("<h", raw, off+20)
            out[name] = (w, h, pc)
    return out


def single_patch_map(tex):
    """Map every multi-patch texture onto a single-patch one of the same size.

    A multi-patch texture has to be composited into a RAM buffer before it can
    be drawn -- a 128x128 one costs 16,408 bytes, which this zone heap cannot
    reliably produce once a level is loaded. Single-patch textures are read
    straight out of the flash-mapped WAD and allocate nothing, so remapping the
    carved map onto them removes the largest allocation in the game.
    """
    singles = [(n, wh) for n, wh in tex.items() if wh[2] == 1]
    remap = {}
    for name, (w, h, pc) in tex.items():
        if pc <= 1:
            continue
        same = [n for n, (sw, sh, _) in singles if sh == h and sw == w]
        if not same:
            same = [n for n, (sw, sh, _) in singles if sh >= h]
        if not same:
            same = [n for n, _ in singles]
        remap[name] = sorted(same)[0] if same else name
    return remap


def unpack_all(data, fmt):
    n = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, data, i) for i in range(0, len(data) - n + 1, n)]


def load_map(wad, name):
    names = [n.upper() for n, _, _ in wad.lumps]
    i = names.index(name.upper())
    out = {}
    for j in range(i + 1, i + 11):
        n, pos, size = wad.lumps[j]
        out[n.upper()] = wad.data[pos:pos + size]
    return out


def carve(m, max_sectors, texremap=None):
    verts = unpack_all(m["VERTEXES"], "<hh")
    lines = unpack_all(m["LINEDEFS"], "<HHhhhHH")
    sides = unpack_all(m["SIDEDEFS"], "<hh8s8s8sh")
    sects = unpack_all(m["SECTORS"],  "<hh8s8shhh")
    things = unpack_all(m["THINGS"],  "<hhhhh")

    # Which sectors touch which, through two-sided lines.
    neigh = {i: set() for i in range(len(sects))}
    for (v1, v2, flags, spec, tag, right, left) in lines:
        if right == 0xFFFF or left == 0xFFFF:
            continue
        a, b = sides[right][5], sides[left][5]
        if a != b:
            neigh[a].add(b)
            neigh[b].add(a)

    # Start where the player does.
    start = next((t for t in things if t[3] == 1), None)
    if start is None:
        raise SystemExit("no player 1 start in this map")

    def sector_of_point(px, py):
        # Nearest line, then whichever side the point falls on.
        best, best_d = None, None
        for (v1, v2, flags, spec, tag, right, left) in lines:
            x1, y1 = verts[v1]; x2, y2 = verts[v2]
            dx, dy = x2 - x1, y2 - y1
            if dx == 0 and dy == 0:
                continue
            t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
            cx, cy = x1 + t * dx, y1 + t * dy
            d = (px - cx) ** 2 + (py - cy) ** 2
            if best_d is None or d < best_d:
                side = (dx * (py - y1) - dy * (px - x1))
                sd = right if side < 0 else (left if left != 0xFFFF else right)
                if sd != 0xFFFF:
                    best, best_d = sides[sd][5], d
        return best

    seed = sector_of_point(start[0], start[1])

    # Breadth-first so the kept region grows outward from the start.
    keep, frontier = {seed}, [seed]
    while frontier and len(keep) < max_sectors:
        nxt = []
        for s in frontier:
            for t in sorted(neigh[s]):
                if t not in keep and len(keep) < max_sectors:
                    keep.add(t)
                    nxt.append(t)
        if not nxt:
            break
        frontier = nxt

    # Keep lines with at least one kept side; seal the ones that led away.
    new_lines, new_sides, boundary = [], [], []
    side_map = {}

    def take_side(idx, seal_middle=False):
        if idx == 0xFFFF:
            return 0xFFFF
        key = (idx, seal_middle)
        if key in side_map:
            return side_map[key]
        xo, yo, up, lo, mid, sec = sides[idx]
        if seal_middle and mid.rstrip(b"\0-") == b"":
            mid = up if up.rstrip(b"\0-") else SEAL_TEX.encode().ljust(8, b"\0")
        side_map[key] = len(new_sides)
        new_sides.append((xo, yo, up, lo, mid, sec))
        return side_map[key]

    for (v1, v2, flags, spec, tag, right, left) in lines:
        rs = sides[right][5] if right != 0xFFFF else None
        ls = sides[left][5] if left != 0xFFFF else None
        r_in, l_in = rs in keep, ls in keep
        if not r_in and not l_in:
            continue
        if r_in and l_in:
            new_lines.append((v1, v2, flags, spec, tag,
                              take_side(right), take_side(left)))
        else:
            # One side survives: turn it into a solid wall.
            if r_in:
                keepside, = (right,)
            else:
                keepside = left
                v1, v2 = v2, v1          # flip so the sector stays on the right
            flags = (flags & ~ML_TWOSIDED) | ML_BLOCKING
            new_lines.append((v1, v2, flags, 0, 0, take_side(keepside, True), 0xFFFF))
            boundary.append((v1, v2))

    # Swap any multi-patch wall texture for a single-patch one.
    if texremap:
        def swap(t):
            k = t.rstrip(b"\0").decode("ascii", "replace").upper()
            r = texremap.get(k)
            return r.encode().ljust(8, b"\0") if r else t
        new_sides = [(xo, yo, swap(up), swap(lo), swap(mid), sec)
                     for (xo, yo, up, lo, mid, sec) in new_sides]

    # Renumber sectors, then sidedefs' sector references.
    sec_map = {s: i for i, s in enumerate(sorted(keep))}
    new_sides = [(xo, yo, up, lo, mid, sec_map[sec]) for (xo, yo, up, lo, mid, sec) in new_sides]
    new_sects = [sects[s] for s in sorted(keep)]

    # Drop unreferenced vertexes.
    used = sorted({v for ln in new_lines for v in ln[:2]})
    vmap = {v: i for i, v in enumerate(used)}
    new_verts = [verts[v] for v in used]
    new_lines = [(vmap[ln[0]], vmap[ln[1]]) + tuple(ln[2:]) for ln in new_lines]

    # A thing is kept when it lies inside the sealed boundary.
    def inside(px, py):
        crossings = 0
        for (a, b) in boundary:
            x1, y1 = verts[a]; x2, y2 = verts[b]
            if (y1 > py) != (y2 > py):
                xi = x1 + (py - y1) * (x2 - x1) / (y2 - y1)
                if xi > px:
                    crossings += 1
        return crossings % 2 == 1

    new_things = [t for t in things if t[3] == 1 or inside(t[0], t[1])]

    return dict(
        VERTEXES=b"".join(struct.pack("<hh", *v) for v in new_verts),
        LINEDEFS=b"".join(struct.pack("<HHhhhHH", *l) for l in new_lines),
        SIDEDEFS=b"".join(struct.pack("<hh8s8s8sh", *s) for s in new_sides),
        SECTORS=b"".join(struct.pack("<hh8s8shhh", *s) for s in new_sects),
        THINGS=b"".join(struct.pack("<hhhhh", *t) for t in new_things),
    ), len(keep), len(sects)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--sectors", type=int, default=30)
    ap.add_argument("--zdbsp", default=os.path.expanduser(
        "/private/tmp/claude-501/-Users-tq-Documents-GitHub-doom-on-htn-badge/"
        "71c78d64-9243-4c36-a33e-68fb1d0236ee/scratchpad/nb/build/zdbsp"))
    a = ap.parse_args()

    wad = Wad(a.src)
    m = load_map(wad, a.map)
    tex = texture_table(wad)
    remap = single_patch_map(tex)
    carved, kept, total = carve(m, a.sectors, remap)
    print(f"  {len(remap)} of {len(tex)} textures are multi-patch; "
          f"remapped to single-patch equivalents")
    print(f"{a.map}: keeping {kept} of {total} sectors, "
          f"{len(carved['LINEDEFS'])//14} of {len(m['LINEDEFS'])//14} linedefs")

    # zdbsp wants a real wad, so hand it a one-map PWAD.
    tmp_in = "/tmp/_cut_in.wad"; tmp_out = "/tmp/_cut_out.wad"
    order = ["THINGS","LINEDEFS","SIDEDEFS","VERTEXES","SEGS","SSECTORS",
             "NODES","SECTORS","REJECT","BLOCKMAP"]
    lumps = [(a.map, b"")] + [(n, carved.get(n, b"")) for n in order]
    open(tmp_in, "wb").write(build_wad(lumps))

    r = subprocess.run([a.zdbsp, "-R", tmp_in, "-o", tmp_out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr); return 1

    built = load_map(Wad(tmp_out), a.map)
    counts = {
        "vertexes": len(built["VERTEXES"])//4, "linedefs": len(built["LINEDEFS"])//14,
        "sidedefs": len(built["SIDEDEFS"])//30, "sectors": len(built["SECTORS"])//26,
        "segs": len(built["SEGS"])//12, "ssectors": len(built["SSECTORS"])//4,
        "nodes": len(built["NODES"])//28,
    }
    ram = sum(counts[k]*COST[k] for k in counts)
    ram += len(built.get("BLOCKMAP", b"")) + len(built.get("REJECT", b""))
    print("  " + "  ".join(f"{k}={v}" for k, v in counts.items()))
    print(f"\n  estimated badge RAM: {int(ram):,} bytes   (budget about 67,332)")
    if ram > 60000:
        print("  ** over budget, try fewer --sectors **")

    # Splice into the badge WAD in place of the original map.
    out, i, src_lumps = [], 0, strip_wad(wad)
    while i < len(src_lumps):
        n, d = src_lumps[i]
        if n.upper() == a.map.upper():
            out.append((n, b""))
            out.extend((k, built.get(k, b"")) for k in order)
            i += 11
            continue
        out.append((n, d)); i += 1
    img = build_wad(out)
    open(a.dst, "wb").write(img)
    print(f"\n{a.dst}: {len(img):,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
