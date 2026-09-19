#include "wad.h"

#include <string.h>
#include <ctype.h>
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "wad";

typedef struct {
    char identification[4];     // "IWAD" or "PWAD"
    int32_t numlumps;
    int32_t infotableofs;
} __attribute__((packed)) wadinfo_t;

typedef struct {
    int32_t filepos;
    int32_t size;
    char name[8];
} __attribute__((packed)) filelump_t;

static const uint8_t *s_base;           // mapped base of the whole partition
static const filelump_t *s_dir;
static int s_numlumps;
static char s_id[5];
static esp_partition_mmap_handle_t s_map;

bool wad_mount(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "wad");
    if (!part) {
        ESP_LOGE(TAG, "no 'wad' partition in the table");
        return false;
    }

    const void *ptr;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &ptr, &s_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return false;
    }
    s_base = ptr;

    const wadinfo_t *hdr = (const wadinfo_t *)s_base;
    if (memcmp(hdr->identification, "IWAD", 4) != 0 &&
        memcmp(hdr->identification, "PWAD", 4) != 0) {
        ESP_LOGE(TAG, "no WAD magic at the start of the partition "
                      "(found %02x %02x %02x %02x) -- has it been flashed?",
                 s_base[0], s_base[1], s_base[2], s_base[3]);
        return false;
    }

    memcpy(s_id, hdr->identification, 4);
    s_id[4] = 0;
    s_numlumps = hdr->numlumps;
    s_dir = (const filelump_t *)(s_base + hdr->infotableofs);

    ESP_LOGI(TAG, "mounted %s: %d lumps, directory at 0x%lx, partition %lu KB",
             s_id, s_numlumps, (unsigned long)hdr->infotableofs,
             (unsigned long)part->size / 1024);
    ESP_LOGI(TAG, "mapped read-only at %p -- lumps are read in place, not copied",
             s_base);
    return true;
}

int wad_num_lumps(void) { return s_numlumps; }
const char *wad_id(void) { return s_id; }

bool wad_find(const char *name, wad_lump_t *out)
{
    char want[9] = {0};
    for (int i = 0; i < 8 && name[i]; i++) want[i] = toupper((unsigned char)name[i]);

    // Doom searches backwards so a later lump of the same name wins.
    for (int i = s_numlumps - 1; i >= 0; i--) {
        char have[9] = {0};
        for (int j = 0; j < 8; j++) have[j] = toupper((unsigned char)s_dir[i].name[j]);
        if (memcmp(have, want, 8) != 0) continue;

        out->name = s_dir[i].name;
        out->data = s_base + s_dir[i].filepos;
        out->size = s_dir[i].size;
        return true;
    }
    return false;
}

bool wad_lump_at(int index, wad_lump_t *out)
{
    if (index < 0 || index >= s_numlumps) return false;
    out->name = s_dir[index].name;
    out->data = s_base + s_dir[index].filepos;
    out->size = s_dir[index].size;
    return true;
}

void wad_draw_patch(const patch_t *p, uint8_t *dst, int dst_w, int dst_h,
                    int x, int y)
{
    for (int col = 0; col < p->width; col++) {
        int dx = x + col;
        if (dx < 0 || dx >= dst_w) continue;

        const uint8_t *post = (const uint8_t *)p + p->columnofs[col];
        // Each post is: topdelta, length, pad, pixels[length], pad.
        while (*post != 0xFF) {
            int topdelta = post[0];
            int len      = post[1];
            const uint8_t *src = post + 3;      // skip the leading pad byte

            for (int i = 0; i < len; i++) {
                int dy = y + topdelta + i;
                if (dy >= 0 && dy < dst_h) dst[dy * dst_w + dx] = src[i];
            }
            post += len + 4;                    // trailing pad byte too
        }
    }
}
