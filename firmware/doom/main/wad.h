#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The WAD lives in its own flash partition and is reached through the flash
// cache as ordinary memory. Nothing is copied to RAM -- on a board with 188 KB
// of contiguous heap, lump data has to stay where it is and be read in place.

typedef struct {
    const char *name;       // 8 chars, not NUL-terminated in the file
    const uint8_t *data;    // pointer directly into mapped flash
    uint32_t size;
} wad_lump_t;

// Doom's picture format: columns of vertical runs, so transparent gaps cost
// nothing. Offsets are relative to the start of the patch.
typedef struct {
    int16_t width, height;
    int16_t leftoffset, topoffset;
    uint32_t columnofs[];
} __attribute__((packed)) patch_t;

bool wad_mount(void);
int  wad_num_lumps(void);
const char *wad_id(void);

// Returns false when the lump is absent. `name` is matched case-insensitively
// against the 8-byte field, as Doom itself does.
bool wad_find(const char *name, wad_lump_t *out);

// Draws a patch into an 8bpp buffer at (x, y), honouring transparency.
void wad_draw_patch(const patch_t *p, uint8_t *dst, int dst_w, int dst_h,
                    int x, int y);
