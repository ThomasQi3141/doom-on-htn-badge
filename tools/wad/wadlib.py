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
DROPPABLE = {"ENDOOM", "DEMO1", "DEMO2", "DEMO3", "DEMO4"}


def is_audio(name):
    u = name.upper()
    return u.startswith(AUDIO_PREFIXES)


def strip_wad(wad, drop_audio=True, drop_extras=True):
    """Return (name, data) pairs with the badge's dead weight removed.

    The board has no speaker, amp or DAC, so every sound, every piece of music
    and every PC-speaker lump is unreachable code's unreachable data. ENDOOM is
    the DOS text screen shown on exit and the demos are attract-mode playback,
    neither of which a badge build needs.
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
