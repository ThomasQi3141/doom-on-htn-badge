#!/usr/bin/env python3
"""Build a purpose-made Doom arena for the badge demo.

Design intent: someone picks the badge up at a booth and is fighting within
five seconds, without ever wondering where to go.

  - Spawn at the end of the south arm looking north, so the entire arena and
    most of its occupants are visible from the first frame. The co-op starts
    sit alongside it, so two paired badges begin shoulder to shoulder.
  - A shotgun sits a few steps ahead. The first thing anyone does is grab it.
  - Four arms radiate from a central chamber, each holding a weapon and its own
    enemies, so moving forward is always rewarded and never ambiguous.
  - A raised platform in the middle carries the armour and doubles as cover,
    and gives the renderer some vertical relief to show off.
  - A pillar in each arm breaks the sightlines so the space reads as
    architecture rather than a box.

Geometry is written out directly and the BSP is built with zdbsp, so no node
builder is needed here. Everything is validated against the geometry before the
WAD is written -- placing a monster inside a pillar is silent in Doom and very
annoying to debug on hardware.
"""
import sys, os, struct, subprocess, argparse, math, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wadlib import Wad, strip_wad, build_wad, decode_patch_into, encode_patch

# --- dimensions -----------------------------------------------------------
ARM   = 1088          # how far each arm reaches from the centre
HALF  = 256           # half-width of an arm (arms are 512 wide)
PLAT  = 160           # half-size of the centre platform
PILL  = 88            # half-size of an arm pillar
PILL_D = 640          # how far along each arm its pillar sits

FLOOR_H, CEIL_H = 0, 200
PLAT_H = 32

WALL_TEX  = "STARTAN3"
PILL_TEX  = "SUPPORT2"
STEP_TEX  = "METAL1"
FLOOR_F, CEIL_F = "FLOOR4_8", "CEIL3_5"
PLAT_F          = "FLAT14"

# --- thing types present in the shareware IWAD ----------------------------
# Thing type 1..4 is the start for player 1..4, and 11 is a deathmatch spot.
# Doom refuses to load a level in which a player in the game has no start, so
# 2..4 are here even though the badge only ever seats two: a map that works
# for co-op and not for a four-way is a trap for later.
PLAYER, PLAYER2, PLAYER3, PLAYER4, DMSTART = 1, 2, 3, 4, 11
SHOTGUN, CHAINGUN, CHAINSAW = 2001, 2002, 2005
ARMOR, MEGAARMOR, MEDIKIT, STIMPACK = 2018, 2019, 2012, 2011
SHELLS, CLIP, AMMOBOX, HEALTHBONUS  = 2008, 2007, 2048, 2014
ZOMBIE, SHOTGUY, IMP, DEMON         = 3004, 9, 3001, 3002
BARREL, LAMP                        = 2035, 2028


def n8(s):
    return s.encode("ascii")[:8].ljust(8, b"\0")


def bam(dx, dy):
    a = int(round(math.atan2(dy, dx) / (2 * math.pi) * 65536)) & 0xFFFF
    return a - 0x10000 if a >= 0x8000 else a


class Map:
    """Vertices, sectors and linedefs, with sidedefs generated as we go."""

    def __init__(self):
        self.verts, self.sectors, self.lines, self.sides = [], [], [], []
        self.things = []

    def v(self, x, y):
        p = (int(x), int(y))
        if p not in self.verts:
            self.verts.append(p)
        return self.verts.index(p)

    def sector(self, floor, ceil, ffl, cfl, light):
        self.sectors.append((floor, ceil, n8(ffl), n8(cfl), light, 0, 0))
        return len(self.sectors) - 1

    def side(self, sec, mid="-", up="-", lo="-"):
        self.sides.append((0, 0, n8(up), n8(lo), n8(mid), sec))
        return len(self.sides) - 1

    def wall(self, a, b, sec, tex):
        """One-sided wall; the sector lies to the right of a->b."""
        self.lines.append((self.v(*a), self.v(*b), 1, 0, 0,
                           self.side(sec, mid=tex), 0xFFFF))

    def step(self, a, b, front, back, lower):
        """Two-sided line where the back sector's floor is higher."""
        self.lines.append((self.v(*a), self.v(*b), 4, 0, 0,
                           self.side(front, lo=lower),
                           self.side(back)))

    def thing(self, x, y, ty, angle=0):
        self.things.append((int(x), int(y), int(angle), ty, 7))


def build():
    m = Map()
    main = m.sector(FLOOR_H, CEIL_H, FLOOR_F, CEIL_F, 160)
    plat = m.sector(PLAT_H, CEIL_H, PLAT_F, CEIL_F, 192)

    # Cross outline, wound so the interior is on the right of every edge.
    o = [(-ARM, -HALF), (-ARM, HALF), (-HALF, HALF), (-HALF, ARM),
         (HALF, ARM), (HALF, HALF), (ARM, HALF), (ARM, -HALF),
         (HALF, -HALF), (HALF, -ARM), (-HALF, -ARM), (-HALF, -HALF)]
    for i in range(len(o)):
        m.wall(o[i], o[(i + 1) % len(o)], main, WALL_TEX)

    # Centre platform: an island, so it winds the other way to keep the main
    # sector on the right.
    p = [(-PLAT, -PLAT), (PLAT, -PLAT), (PLAT, PLAT), (-PLAT, PLAT)]
    for i in range(len(p)):
        m.step(p[i], p[(i + 1) % len(p)], main, plat, STEP_TEX)

    # Solid pillars: same winding as the platform, but one-sided -- there is
    # no sector inside them.
    pillars = [(0, PILL_D), (0, -PILL_D), (PILL_D, 0), (-PILL_D, 0)]
    for (cx, cy) in pillars:
        q = [(cx - PILL, cy - PILL), (cx + PILL, cy - PILL),
             (cx + PILL, cy + PILL), (cx - PILL, cy + PILL)]
        for i in range(len(q)):
            m.wall(q[i], q[(i + 1) % len(q)], main, PILL_TEX)

    # --- population -------------------------------------------------------
    # Facing north, at the far end of the south arm: the whole arena is in
    # view on the first frame.
    m.thing(0, -ARM + 140, PLAYER, 90)

    # Co-op: the second badge spawns beside the first, facing the same way, so
    # the two players see each other immediately and share the same opening
    # view of the arena. Players 3 and 4 fill in behind them -- the badge pairs
    # two at a time, but a start missing for a player in the game is a refused
    # level, not a missing player.
    m.thing(-130, -ARM + 140, PLAYER2, 90)
    m.thing(130, -ARM + 140, PLAYER3, 90)
    m.thing(0, -ARM + 268, PLAYER4, 90)

    # Deathmatch spots, one at the end of each arm: co-op is what the badge
    # offers today, but G_DeathMatchSpawnPlayer is fatal without them.
    for (dx, dy, da) in ((0, -900, 90), (0, 900, 270), (900, 0, 180),
                         (-900, 0, 0)):
        m.thing(dx, dy, DMSTART, da)

    # Reachable before anything closes in.
    m.thing(0, -430, SHOTGUN)
    m.thing(-70, -430, SHELLS)
    m.thing(70, -430, SHELLS)

    # Rewards for pushing down each arm.
    m.thing(0, ARM - 150, CHAINGUN)
    m.thing(-80, ARM - 150, CLIP)
    m.thing(80, ARM - 150, CLIP)
    m.thing(-ARM + 150, 0, CHAINSAW)
    m.thing(ARM - 150, 0, MEDIKIT)
    m.thing(ARM - 150, 90, AMMOBOX)

    # On the platform, so it is visible from everywhere and worth climbing.
    m.thing(0, 0, ARMOR)
    m.thing(-90, 0, HEALTHBONUS)
    m.thing(90, 0, HEALTHBONUS)

    # The diagonals are outside a plus shape -- keep everything on an axis.
    m.thing(-180, -420, STIMPACK)
    m.thing(180, -420, STIMPACK)
    m.thing(-200, 430, MEDIKIT)

    # Barrels near the pillars: chain explosions read well on a small screen.
    for (cx, cy) in [(0, PILL_D), (PILL_D, 0), (-PILL_D, 0)]:
        m.thing(cx + 150, cy + 150, BARREL)
        m.thing(cx - 150, cy - 150, BARREL)

    # Enemies: a pair down each arm plus melee in the middle. Imps go north so
    # they are the first thing seen from the spawn.
    m.thing(-150, 880, IMP, 270)
    m.thing(150, 880, IMP, 270)
    m.thing(880, 150, ZOMBIE, 180)
    m.thing(880, -150, ZOMBIE, 180)
    m.thing(-880, 150, SHOTGUY, 0)
    m.thing(-880, -150, SHOTGUY, 0)
    m.thing(-180, 420, DEMON, 270)
    m.thing(180, 420, DEMON, 270)
    m.thing(-430, -150, IMP, 0)
    m.thing(430, -150, IMP, 180)

    return m, main, plat


def validate(m):
    """Catch anything placed inside geometry before it reaches hardware."""
    problems = []

    def in_cross(x, y):
        return ((abs(x) <= ARM and abs(y) <= HALF) or
                (abs(y) <= ARM and abs(x) <= HALF))

    pillars = [(0, PILL_D), (0, -PILL_D), (PILL_D, 0), (-PILL_D, 0)]
    for (x, y, ang, ty, fl) in m.things:
        if not in_cross(x, y):
            problems.append(f"thing {ty} at ({x},{y}) is outside the arena")
            continue
        # Doom's biggest radius here is the demon's 30; keep well clear.
        for (cx, cy) in pillars:
            if abs(x - cx) < PILL + 40 and abs(y - cy) < PILL + 40:
                problems.append(f"thing {ty} at ({x},{y}) overlaps the pillar "
                                f"at ({cx},{cy})")
        for edge, lim in ((abs(x), ARM), (abs(y), ARM)):
            pass
        if in_cross(x, y):
            # distance to the nearest arm wall
            if abs(y) <= HALF:
                clear_x = ARM - abs(x)
            else:
                clear_x = HALF - abs(x)
            if abs(x) <= HALF:
                clear_y = ARM - abs(y)
            else:
                clear_y = HALF - abs(y)
            if min(clear_x, clear_y) < 40:
                problems.append(f"thing {ty} at ({x},{y}) is {min(clear_x,clear_y)} "
                                f"from a wall")

    # Co-op starts have to be far enough apart that every player can stand on
    # one at once: P_SpawnPlayer does not check, and a co-op game that
    # telefrags one of its two players on the first tic is a very confusing
    # bug to meet on hardware. Doom's player radius is 16, so 64 is
    # comfortable. Deathmatch spots are left out of the comparison: they are
    # only ever used in a game where the co-op starts are not.
    starts = [(x, y, ty) for (x, y, a, ty, f) in m.things
              if ty in (PLAYER, PLAYER2, PLAYER3, PLAYER4)]
    for i, (x1, y1, t1) in enumerate(starts):
        for (x2, y2, t2) in starts[i + 1:]:
            if abs(x1 - x2) < 64 and abs(y1 - y2) < 64:
                problems.append(f"starts {t1} at ({x1},{y1}) and {t2} at "
                                f"({x2},{y2}) are on top of each other")

    for want in (PLAYER, PLAYER2, PLAYER3, PLAYER4):
        if not any(ty == want for (_, _, ty) in starts):
            problems.append(f"no start for player {want}")

    return problems


def emit(m):
    VERTEXES = b"".join(struct.pack("<hh", x, y) for x, y in m.verts)
    THINGS   = b"".join(struct.pack("<hhhhh", *t) for t in m.things)
    LINEDEFS = b"".join(struct.pack("<HHhhhHH", *l) for l in m.lines)
    SIDEDEFS = b"".join(struct.pack("<hh8s8s8sh", *s) for s in m.sides)
    SECTORS  = b"".join(struct.pack("<hh8s8shhh", *s) for s in m.sectors)
    return dict(THINGS=THINGS, LINEDEFS=LINEDEFS, SIDEDEFS=SIDEDEFS,
                VERTEXES=VERTEXES, SECTORS=SECTORS,
                SEGS=b"", SSECTORS=b"", NODES=b"", REJECT=b"", BLOCKMAP=b"")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--zdbsp", default=os.environ.get("ZDBSP") or
                    shutil.which("zdbsp"),
                    help="node builder (default: $ZDBSP, then PATH)")
    a = ap.parse_args()
    if not a.zdbsp:
        sys.exit("zdbsp not found: pass --zdbsp, set $ZDBSP, or put it on PATH")

    m, main_s, plat_s = build()
    bad = validate(m)
    print(f"arena: {len(m.verts)} vertices, {len(m.lines)} linedefs, "
          f"{len(m.sectors)} sectors, {len(m.things)} things")
    if bad:
        print("  PLACEMENT PROBLEMS:")
        for b in bad:
            print(f"    {b}")
        return 1
    print("  all things validated clear of walls and pillars")

    order = ["THINGS","LINEDEFS","SIDEDEFS","VERTEXES","SEGS","SSECTORS",
             "NODES","SECTORS","REJECT","BLOCKMAP"]
    raw = emit(m)
    tmp_in, tmp_out = "/tmp/_arena_in.wad", "/tmp/_arena_out.wad"
    open(tmp_in, "wb").write(build_wad([(a.map, b"")] + [(k, raw[k]) for k in order]))

    r = subprocess.run([a.zdbsp, "-R", tmp_in, "-o", tmp_out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr); return 1

    built_wad = Wad(tmp_out)
    bn = [n.upper() for n, _, _ in built_wad.lumps]
    bi = bn.index(a.map.upper())
    built = {}
    for j in range(bi + 1, bi + 11):
        n, pos, size = built_wad.lumps[j]
        built[n.upper()] = built_wad.data[pos:pos + size]

    counts = {k: len(built[k]) // s for k, s in
              (("VERTEXES",4),("LINEDEFS",14),("SIDEDEFS",30),("SECTORS",26),
               ("SEGS",12),("SSECTORS",4),("NODES",28))}
    cost = {"VERTEXES":12.8,"LINEDEFS":111,"SIDEDEFS":32.5,"SECTORS":148,
            "SEGS":40,"SSECTORS":8,"NODES":56}
    ram = sum(counts[k]*cost[k] for k in counts) + len(built["BLOCKMAP"]) + len(built["REJECT"])
    print("  " + "  ".join(f"{k.lower()}={v}" for k, v in counts.items()))
    print(f"  estimated badge RAM: {int(ram):,} bytes (budget about 67,332)")

    # Package exactly as the carved maps are: flatten multi-patch textures into
    # flash so nothing has to be composited at run time, prune TEXTURE1 to what
    # this map can reach, and drop the levels we cannot load anyway.
    from cutmap import flatten_textures, prune_texture1, texture_table
    wad = Wad(a.src)
    tex = texture_table(wad)

    used = set()
    sd = built["SIDEDEFS"]
    for k in range(0, len(sd) - 29, 30):
        for off in (4, 12, 20):
            t = sd[k+off:k+off+8].rstrip(b"\0").decode("ascii", "replace").upper()
            if t and t != "-":
                used.add(t)

    flat_lumps, pnames_out, flattened, report = flatten_textures(wad, used, tex)
    for nm, w, h, pc, cov, tot in report:
        print(f"  pre-composited {nm} ({pc} patches -> 1, {100.0*cov/tot:.0f}% covered)")
    tex1, kept_tex, all_tex = prune_texture1(wad, used, flattened)
    print(f"  TEXTURE1 {all_tex} -> {kept_tex} textures")

    out, src_lumps, i = [], strip_wad(wad), 0
    while i < len(src_lumps):
        n, d = src_lumps[i]
        u = n.upper()
        if u == a.map.upper():
            out.append((n, b""))
            out.extend((k, built.get(k, b"")) for k in order)
            i += 11
            continue
        if len(u) == 4 and u[0] == "E" and u[2] == "M":
            i += 11                      # a level we cannot load; drop it
            continue
        out.append((n, tex1 if u == "TEXTURE1" else
                       (pnames_out if u == "PNAMES" else d)))
        i += 1

    if flat_lumps:
        idx = next((j for j, (n, _) in enumerate(out) if n.upper() == "P_END"),
                   len(out))
        out = out[:idx] + flat_lumps + out[idx:]

    img = build_wad(out)
    open(a.dst, "wb").write(img)
    print(f"\n{a.dst}: {len(img):,} bytes, {len(out)} lumps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
