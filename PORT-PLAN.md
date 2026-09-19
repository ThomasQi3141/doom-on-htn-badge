# Porting Doom to the badge: the RAM budget

Flash is solved (see `HARDWARE.md`): the whole shareware episode fits
uncompressed. What remains is RAM, and the numbers below are measured, not
estimated — doomgeneric's sources compiled with the badge's own toolchain,
`riscv32-esp-elf-gcc -march=rv32imc -mabi=ilp32 -Os`.

## What we have

```
total DRAM available        322,404 bytes
largest contiguous block    188,416 bytes   <- three disjoint heap regions
```

## What stock Doom needs

83 of doomgeneric's translation units, built for this target:

| section | bytes | lands in |
|---|---|---|
| `.text` | 264,210 | flash — fits the 640 KB app partition easily |
| `.data` | 60,072 | **RAM** |
| `.bss` | 243,752 | **RAM** |

**Statics alone are 303,824 bytes against 322,404 available.** That leaves
18,580 bytes for the framebuffer, the zone heap and every stack — so the port is
over budget before Doom allocates a single byte. On top of that, doomgeneric
hardcodes `DEFAULT_RAM = MIN_RAM = 6 MiB` for the zone.

### Where the `.bss` actually goes

| object | bss | what it is |
|---|---|---|
| `r_plane.o` | **134,152** | 128 visplanes (664 B each) + `MAXOPENINGS` = 320×64 shorts |
| `r_main.o` | 29,240 | light/scale lookup tables |
| `d_loop.o` | 20,580 | netgame tic buffers |
| `r_bsp.o` | 12,572 | 256 drawsegs |
| `r_things.o` | 11,224 | 128 vissprites |
| `r_draw.o` | 8,688 | column/span scratch |
| `statdump.o` | 6,404 | end-of-level stats dump |

## The target budget

```
framebuffer (320x200, 8bpp)      64,000   fixed, already built and proven
DMA chunk buffers                20,480   already built and proven
FreeRTOS + IDF + stacks         ~25,000
                                -------
available to Doom               ~213,000
  statics                       ~120,000   (down from 303,824)
  zone heap                      ~90,000
```

**Savings required: roughly 184,000 bytes of statics.**

## Where the savings come from

| change | saves | note |
|---|---|---|
| Shrink visplanes & openings | ~114,000 | `MAXVISPLANES` 128→32, `MAXOPENINGS` 320×64→320×16 |
| Cut `d_loop` netgame buffers | 20,580 | no multiplayer on a single badge |
| Cut `r_main` light tables | ~17,000 | compute on the fly or store coarser |
| Cut drawsegs 256→96 | ~8,000 | |
| Cut vissprites 128→48 | ~7,000 | |
| Cut `statdump` | 6,404 | delete outright |
| | **~173,000** | close to target; `.data` trimming covers the rest |

Visplane and drawseg limits are the classic Doom overflow points, so cutting
them is not free — complex scenes will drop geometry rather than crash if the
overflow paths are handled. rp2040-doom rewrote this area rather than tuning
constants, and we may end up doing the same.

## The lever that makes the zone heap plausible

Vanilla Doom's zone heap is dominated by `W_CacheLumpNum` copying lumps out of
the WAD file into RAM. **Our WAD is memory-mapped through the flash cache**, so
`W_CacheLumpNum` can return a pointer straight into flash and allocate nothing.
That removes the single largest zone consumer, and it is a luxury the RP2040
port had to work much harder for.

Level geometry is the remaining question: `P_Load*` converts on-disk structures
into wider in-memory ones. Those conversions have to either stay flash-resident
with on-the-fly widening, or be budgeted for explicitly — E1M1 is 55,714 bytes
on disk and the loaded form is larger.

## Order of work

1. Strip what the badge cannot use: sound, music, netgame, demos, statdump.
2. Re-measure. The cuts above are mechanical and should be verified before any
   rewriting starts.
3. Shrink the renderer's static arrays and handle the overflow paths.
4. Point `W_CacheLumpNum` at mapped flash; size the zone to what is left.
5. Wire the platform layer to the framebuffer and button map that already work.
