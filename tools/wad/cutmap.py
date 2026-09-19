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
from wadlib import Wad, strip_wad, build_wad, decode_patch_into, encode_patch

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
    # Round-robin through the candidates so distinct source textures keep
    # distinct replacements. Collapsing them all onto one texture fits just as
    # well but makes every wall in the level look identical.
    rr = {}
    for name in sorted(tex):
        w, h, pc = tex[name]
        if pc <= 1:
            continue
        same = sorted(n for n, (sw, sh, _) in singles if sh == h and sw == w)
        if not same:
            same = sorted(n for n, (sw, sh, _) in singles if sh >= h)
        if not same:
            same = sorted(n for n, _ in singles)
        if not same:
            continue
        k = tuple(same)
        rr[k] = rr.get(k, -1) + 1
        remap[name] = same[rr[k] % len(same)]
    return remap


def flatten_textures(wad, used, tex):
    """Pre-composite multi-patch textures into single-patch ones.

    A multi-patch texture has to be assembled into a RAM buffer before Doom can
    draw it -- 16,408 bytes for a 128x128, and this map wants seven of them,
    76 KB against a zone that has about 18 KB free. Substituting single-patch
    textures avoids that but puts doors and decorative faces on ordinary walls.

    Doing the compositing here instead keeps the exact original appearance and
    costs nothing at run time: the result is one patch, which R_GetColumn reads
    straight out of flash.
    """
    pnames_raw = wad.find("PNAMES")
    npat, = struct.unpack_from("<i", pnames_raw, 0)
    pnames = [pnames_raw[4+i*8:12+i*8].rstrip(b"\0").decode("ascii","replace").upper()
              for i in range(npat)]

    raw = wad.find("TEXTURE1")
    n, = struct.unpack_from("<i", raw, 0)
    offs = struct.unpack_from(f"<{n}i", raw, 4)

    new_lumps, new_names, flattened, report = [], [], {}, []
    for off in offs:
        name = raw[off:off+8].rstrip(b"\0").decode("ascii","replace").upper()
        if name not in used:
            continue
        w, h = struct.unpack_from("<hh", raw, off+12)
        pc, = struct.unpack_from("<h", raw, off+20)
        if pc <= 1:
            continue

        pix  = bytearray(w*h)
        mask = bytearray(w*h)
        for k in range(pc):
            ox, oy, pidx, _sd, _cm = struct.unpack_from("<hhhhh", raw, off+22+k*10)
            praw = wad.find(pnames[pidx])
            if praw is None:
                continue
            decode_patch_into(praw, pix, w, h, ox, oy, mask)

        covered = sum(mask)
        report.append((name, w, h, pc, covered, w*h))
        # Walls are opaque: treat any gap as solid so no column ends up
        # patchless, which R_GenerateLookup refuses to draw.
        for i in range(w*h):
            mask[i] = 1

        lumpname = ("BT%04d" % len(new_lumps))
        new_lumps.append((lumpname, encode_patch(pix, w, h, mask)))
        new_names.append(lumpname)
        flattened[name] = len(pnames) + len(new_names) - 1

    pnames_out = struct.pack("<i", len(pnames) + len(new_names)) + \
        b"".join(n.encode().ljust(8, b"\0") for n in pnames + new_names)
    return new_lumps, pnames_out, flattened, report


def prune_texture1(wad, keep_names, flattened=None):
    """Rebuild TEXTURE1 containing only the named textures.

    R_InitTextures allocates seven per-texture arrays plus a texture_t for
    every entry in TEXTURE1 -- about 13,200 bytes for the full 125 on this
    board, spent before a level is even chosen. A carved map references a
    handful. P_InitSwitchList and P_InitPicAnims both use
    R_CheckTextureNumForName and skip what is missing, so dropping the rest is
    safe; SKY1 is kept because R_InitSkyMap fetches it by name and aborts.
    """
    raw = wad.find("TEXTURE1")
    n, = struct.unpack_from("<i", raw, 0)
    offs = struct.unpack_from(f"<{n}i", raw, 4)
    want = {k.upper() for k in keep_names} | {"SKY1"}

    # Index 0 is Doom's "no texture" marker -- R_CheckTextureNumForName returns
    # 0 for "-", and R_RenderSegLoop skips any wall whose texture is 0. The
    # original table reserves that slot with a dummy (AASTINKY) that no level
    # references. Dropping it puts a real texture at index 0, and every wall
    # using it silently stops being drawn -- which looks exactly like the
    # renderer failing to paint, not like a texture problem.
    entries = []
    first_off = offs[0]
    first_pc, = struct.unpack_from("<h", raw, first_off + 20)
    entries.append(raw[first_off:first_off + 22 + first_pc * 10])
    first_name = raw[first_off:first_off+8].rstrip(b"\0").decode("ascii", "replace").upper()

    for off in offs:
        name = raw[off:off+8].rstrip(b"\0").decode("ascii", "replace").upper()
        if name not in want or name == first_name:
            continue
        pc, = struct.unpack_from("<h", raw, off + 20)
        if flattened and name in flattened:
            # rewrite as a single patch pointing at the pre-composited lump
            head = bytearray(raw[off:off + 22])
            struct.pack_into("<h", head, 20, 1)
            entries.append(bytes(head) +
                           struct.pack("<hhhhh", 0, 0, flattened[name], 1, 0))
        else:
            entries.append(raw[off:off + 22 + pc * 10])

    header = struct.pack("<i", len(entries))
    table_end = 4 + 4 * len(entries)
    out, pos = b"", table_end
    offs_out = b""
    for e in entries:
        offs_out += struct.pack("<i", pos)
        pos += len(e)
    return header + offs_out + b"".join(entries), len(entries), n


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


# Thing types we can safely turn into monsters: decorations and pickups sit on
# valid floor, so their coordinates are known-good spawn points.
FILLER_TYPES = {2014, 2015, 2011, 2012, 2007, 2008, 2028, 15, 18, 20, 21, 2035}
# Everything here has sprites in the shareware IWAD. Cacodemons and the rest
# of the bestiary are episode 2+, so their sprites are absent and spawning one
# aborts in R_InitSprites.
# An even mix. These will fight each other as well as the player -- Doom
# monsters retaliate against whatever damaged them (p_inter.c: "target->target
# = source"), and a ring of them around the player means plenty of crossfire.
# That is deliberate: the infighting is worth watching.
ARENA_MONSTERS = [3004,   # zombieman
                  3001,   # imp
                  9,      # shotgun guy
                  3002]   # demon
ARENA_PICKUPS  = [(2001, "shotgun"), (2008, "shells"), (2008, "shells"),
                  (2012, "medikit"), (2018, "armor")]


def place_arena(new_things, boundary, verts, lines, start, count):
    """Ring monsters and pickups around the player start.

    The demo should open in a fight, not a walk. Candidate spots are taken on
    rings outward from the start, kept only if they sit inside the sealed
    boundary and well clear of any wall, so nothing spawns stuck in geometry.
    """
    import math as _m
    px, py = start[0], start[1]

    def inside(x, y):
        c = 0
        for (a, b) in boundary:
            x1, y1 = verts[a]; x2, y2 = verts[b]
            if (y1 > y) != (y2 > y):
                xi = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                if xi > x:
                    c += 1
        return c % 2 == 1

    def clearance(x, y):
        best = 1e9
        for ln in lines:
            x1, y1 = verts[ln[0]]; x2, y2 = verts[ln[1]]
            dx, dy = x2 - x1, y2 - y1
            if dx == 0 and dy == 0:
                continue
            t = max(0.0, min(1.0, ((x-x1)*dx + (y-y1)*dy) / (dx*dx + dy*dy)))
            cx, cy = x1 + t*dx, y1 + t*dy
            best = min(best, _m.hypot(x-cx, y-cy))
        return best

    spots = []
    # Well back from the spawn: the demo should open with a moment to look
    # around and walk, then a fight, rather than a fight immediately.
    for radius in (560, 640, 720, 800, 880, 960, 1040):
        for k in range(16):
            ang = 2 * _m.pi * k / 16
            x = int(px + radius * _m.cos(ang))
            y = int(py + radius * _m.sin(ang))
            # A demon's radius is 30; 40 units of clearance can wedge one in
            # geometry at spawn, where it never moves and never attacks.
            if inside(x, y) and clearance(x, y) > 56:
                spots.append((x, y))

    placed = 0
    for i, (x, y) in enumerate(spots):
        if placed >= count:
            break
        ty = ARENA_MONSTERS[placed % len(ARENA_MONSTERS)]
        new_things.append((x, y, 0, ty, 7))
        placed += 1

    # A pistol alone makes for a short demo.
    for j, (ty, _name) in enumerate(ARENA_PICKUPS):
        if placed + j < len(spots):
            x, y = spots[len(spots) - 1 - j]
            new_things.append((x, y, 0, ty, 7))

    return placed, len(spots)


def pick_seal_texture(sides, keep, tex):
    """Choose a single-patch texture the map already uses, for sealed walls.

    STARTAN3 was the hardcoded choice and it is multi-patch: 128x128 over two
    patches, so drawing it needs a 16,408-byte composite that a loaded zone
    cannot reliably produce. Every texture the level itself uses is
    single-patch, so borrowing the commonest of them costs nothing to render
    and makes the seals blend in rather than announce themselves.
    """
    from collections import Counter
    c = Counter()
    for (xo, yo, up, lo, mid, sec) in sides:
        if sec not in keep:
            continue
        for t in (mid, up, lo):
            nm = t.rstrip(b"\0").decode("ascii", "replace").upper()
            if nm and nm != "-" and tex.get(nm, (0, 0, 9))[2] == 1:
                c[nm] += 1
    return c.most_common(1)[0][0] if c else SEAL_TEX


def carve(m, max_sectors, texremap=None, add_monsters=0, arena=0, tex=None):
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

    seal_tex = pick_seal_texture(sides, keep, tex or {})

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
            mid = up if up.rstrip(b"\0-") else seal_tex.encode().ljust(8, b"\0")
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

    # The start of E1M1 is nearly empty by design -- the original's 29 monsters
    # are almost all deeper in, in the part being cut away. Without this the
    # fragment has real architecture and nothing in it.
    if add_monsters:
        spots = [t for t in new_things if t[3] in FILLER_TYPES]
        placed = 0
        for i, t in enumerate(spots):
            if placed >= add_monsters:
                break
            x, y, ang, ty, fl = t
            new_things.append((x, y, ang, MONSTERS[placed % len(MONSTERS)], 7))
            placed += 1
        print(f"  added {placed} monsters at existing item positions")

    if arena:
        n, spots = place_arena(new_things, boundary, verts, lines, start, arena)
        kinds = len(set(ARENA_MONSTERS[:n])) if n else 0
        print(f"  arena: {n} monsters of {kinds} types around the player start "
              f"({spots} valid spots found), plus weapons and health")

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
    ap.add_argument("--only-map", action="store_true",
                    help="drop every level except the target, freeing flash")
    ap.add_argument("--keep-textures", action="store_true",
                    help="use the level's real textures instead of single-patch stand-ins")
    ap.add_argument("--arena", type=int, default=0,
                    help="monsters to ring around the player start")
    ap.add_argument("--monsters", type=int, default=0,
                    help="extra monsters to place on known-good floor spots")
    ap.add_argument("--zdbsp", default=os.path.expanduser(
        "/private/tmp/claude-501/-Users-tq-Documents-GitHub-doom-on-htn-badge/"
        "71c78d64-9243-4c36-a33e-68fb1d0236ee/scratchpad/nb/build/zdbsp"))
    a = ap.parse_args()

    wad = Wad(a.src)
    m = load_map(wad, a.map)
    tex = texture_table(wad)
    # Remapping was introduced to avoid composite allocations back when the
    # zone was far tighter and TEXTURE1 still held all 125 entries. It
    # substitutes same-size single-patch textures, which renders without
    # allocating but puts doors and decorative faces on ordinary walls.
    remap = {} if a.keep_textures else single_patch_map(tex)
    carved, kept, total = carve(m, a.sectors, remap, a.monsters, a.arena, tex)
    if remap:
        print(f"  {len(remap)} of {len(tex)} textures are multi-patch; "
              f"remapped to single-patch equivalents")
    else:
        print("  keeping the level's real textures")
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
    # Shrink TEXTURE1 to what this map can reach.
    # Texture names live at offsets 4, 12 and 20 of each 30-byte sidedef.
    # Forgetting to add k here read sidedef 0 over and over, so `used` held one
    # texture, the prune kept three, and every other wall in the level resolved
    # to index 0 -- Doom's "no texture" marker -- and stopped being drawn.
    used = set()
    sd = built["SIDEDEFS"]
    for k in range(0, len(sd) - 29, 30):
        for off in (4, 12, 20):
            t = sd[k+off:k+off+8].rstrip(b"\0").decode("ascii", "replace").upper()
            if t and t != "-":
                used.add(t)
    worst = 0
    for t in used:
        if t in tex and tex[t][2] > 1:
            worst = max(worst, tex[t][0] * tex[t][1] + 24)
    if worst:
        print(f"  largest composite needed: {worst:,} bytes "
              f"(PU_CACHE, so the zone can reclaim it)")

    flat_lumps, pnames_out, flattened, report = flatten_textures(wad, used, tex)
    if report:
        print("  pre-compositing multi-patch textures into flash:")
        for nm, w, h, pc, cov, tot in report:
            pct = 100.0 * cov / tot
            warn = "" if pct > 99.0 else f"   <-- only {pct:.0f}% covered by patches"
            print(f"    {nm:<10} {w:>3}x{h:<3} {pc} patches -> 1{warn}")
    tex1, kept_tex, all_tex = prune_texture1(wad, used, flattened)
    out = [(n, tex1 if n.upper() == "TEXTURE1" else
               (pnames_out if n.upper() == "PNAMES" else d)) for n, d in out]

    # Only E1M1 is reachable, and the other eight levels are ~750 KB of flash
    # that the pre-composited patches need.
    if a.only_map:
        drop, keep2, i2 = set(), [], 0
        names2 = [n.upper() for n, _ in out]
        for j, (n, d) in enumerate(out):
            u = n.upper()
            if len(u) == 4 and u[0] == "E" and u[2] == "M" and u != a.map.upper():
                drop.update(range(j, j + 11))
        out = [(n, d) for j, (n, d) in enumerate(out) if j not in drop]
        print(f"  dropped {len(drop)//11} unused levels ({len(drop)} lumps)")

    # Insert the flattened patches just before P_END so they sit with the
    # other wall patches.
    if flat_lumps:
        idx = next((j for j, (n, _) in enumerate(out) if n.upper() == "P_END"),
                   len(out))
        out = out[:idx] + flat_lumps + out[idx:]
        print(f"  added {len(flat_lumps)} pre-composited patches "
              f"({sum(len(d) for _, d in flat_lumps):,} bytes of flash)")
    print(f"  TEXTURE1 pruned from {all_tex} textures to {kept_tex} "
          f"(saves roughly {int((all_tex-kept_tex)*106):,} bytes of zone)")

    img = build_wad(out)
    open(a.dst, "wb").write(img)
    print(f"\n{a.dst}: {len(img):,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
