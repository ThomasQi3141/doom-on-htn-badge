"""Minimal WAD reader and Doom picture decoder.

Deliberately mirrors firmware/doom/main/wad.c so the two can be compared: the
device prints an FNV-1a hash of its decoded framebuffer and this computes the
same number on the host. If they match, the flash mapping, the lump lookup and
the column decoder are all correct.
"""
import struct


class Wad:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        ident, numlumps, infotableofs = struct.unpack_from("<4sii", self.data, 0)
        if ident not in (b"IWAD", b"PWAD"):
            raise ValueError(f"{path}: not a WAD (magic {ident!r})")
        self.ident = ident.decode()
        self.lumps = []          # (name, filepos, size) in file order
        for i in range(numlumps):
            pos, size, name = struct.unpack_from(
                "<ii8s", self.data, infotableofs + i * 16)
            self.lumps.append((name.rstrip(b"\0").decode("ascii", "replace"),
                               pos, size))

    def find(self, name):
        """Last match wins, as Doom does."""
        name = name.upper()
        for lname, pos, size in reversed(self.lumps):
            if lname.upper() == name:
                return self.data[pos:pos + size]
        return None


def decode_patch(raw, dst_w, dst_h, x=0, y=0, dst=None):
    """Decode Doom's column/post picture format into an 8bpp bytearray."""
    if dst is None:
        dst = bytearray(dst_w * dst_h)
    width, height, leftoffset, topoffset = struct.unpack_from("<hhhh", raw, 0)
    colofs = struct.unpack_from(f"<{width}I", raw, 8)

    for col in range(width):
        dx = x + col
        if not (0 <= dx < dst_w):
            continue
        ptr = colofs[col]
        while raw[ptr] != 0xFF:
            topdelta = raw[ptr]
            length = raw[ptr + 1]
            src = ptr + 3                       # skip the leading pad byte
            for i in range(length):
                dy = y + topdelta + i
                if 0 <= dy < dst_h:
                    dst[dy * dst_w + dx] = raw[src + i]
            ptr += length + 4                   # trailing pad byte too
    return dst, width, height, leftoffset, topoffset


def fnv1a(buf):
    h = 2166136261
    for b in buf:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def build_wad(lumps):
    """Pack (name, bytes) pairs into a valid WAD image."""
    header = 12
    body = b"".join(data for _, data in lumps)
    directory = b""
    pos = header
    for name, data in lumps:
        directory += struct.pack("<ii8s", pos, len(data),
                                 name.upper().encode()[:8].ljust(8, b"\0"))
        pos += len(data)
    return struct.pack("<4sii", b"IWAD", len(lumps), header + len(body)) \
        + body + directory


AUDIO_PREFIXES = ("D_", "DS", "DP")

# Nothing else is safe to drop. The demos are attract-mode content that Doom
# plays by name when the title screen times out, and ENDOOM is fetched by name
# on quit -- both go through W_GetNumForName, which calls I_Error on a miss, so
# removing them turns a timeout into a crash-and-reboot loop.
DROPPABLE = set()


def is_audio(name):
    u = name.upper()
    return u.startswith(AUDIO_PREFIXES)


def strip_wad(wad, drop_audio=True, drop_extras=False):
    """Return (name, data) pairs with the badge's dead weight removed.

    The board has no speaker, amp or DAC, so every sound, every piece of music
    and every PC-speaker lump is unreachable code's unreachable data. That is
    the only category that can go: anything Doom fetches by name at runtime
    must stay, because W_GetNumForName calls I_Error when a lump is absent.
    """
    out = []
    for name, pos, size in wad.lumps:
        u = name.upper()
        if drop_audio and is_audio(u):
            continue
        if drop_extras and u in DROPPABLE:
            continue
        out.append((name, wad.data[pos:pos + size]))
    return out


# ---------------------------------------------------------------- pictures

def decode_patch_into(raw, dst, dst_w, dst_h, ox, oy, mask):
    """Paste a Doom patch into an 8bpp buffer, recording covered pixels."""
    width, height, lo, to = struct.unpack_from("<hhhh", raw, 0)
    colofs = struct.unpack_from(f"<{width}I", raw, 8)
    for col in range(width):
        x = ox + col
        if not (0 <= x < dst_w):
            continue
        p = colofs[col]
        while raw[p] != 0xFF:
            top, ln = raw[p], raw[p + 1]
            src = p + 3
            for i in range(ln):
                y = oy + top + i
                if 0 <= y < dst_h:
                    dst[y * dst_w + x] = raw[src + i]
                    mask[y * dst_w + x] = 1
            p += ln + 4
    return width, height


def encode_patch(pix, w, h, mask=None):
    """Encode an 8bpp buffer as a Doom patch (column/post format)."""
    cols = []
    for x in range(w):
        runs, y = [], 0
        while y < h:
            if mask is not None and not mask[y * w + x]:
                y += 1
                continue
            start = y
            while y < h and (mask is None or mask[y * w + x]) and (y - start) < 254:
                y += 1
            runs.append((start, bytes(pix[r * w + x] for r in range(start, y))))
        col = b""
        for top, data in runs:
            col += bytes([top, len(data), data[0] if data else 0]) + data + bytes([data[-1] if data else 0])
        col += b"\xff"
        cols.append(col)

    header = struct.pack("<hhhh", w, h, 0, 0)
    table = 8 + 4 * w
    offs, pos = b"", table
    for c in cols:
        offs += struct.pack("<I", pos)
        pos += len(c)
    return header + offs + b"".join(cols)
