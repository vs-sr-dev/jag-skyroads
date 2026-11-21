"""Access to the user's own SkyRoads files, and the decoders of their formats.

The source is either the freeware distribution's zip as downloaded, or a folder
holding its files.  File names are matched case-insensitively: the zip carries
them in upper case, other copies in lower case.

Formats per skyroads-port's docs/FORMATS.md (MIT, see tools/lzs.py).  Nothing
read here is ever written into the repository: the converted blobs go to cart/
and the previews to build/, and both are ignored by git.
"""
import os
import struct
import zipfile

from lzs import Stream, decompress

SCREEN_W = 320
SCREEN_H = 200
VIEW_H = 138

# Where each file's CMAP lands in the 256-entry palette the game builds
# (palette bases from the original loader, FORMATS.md sec. 2).
BASE_ROAD = 0       # 72 colours, per road
BASE_CARS = 72      # 20
BASE_DASH = 92      # 50
BASE_WORLD = 142    # 114, per world

N_ROADS = 31        # entry 0 = attract demo road, 1..30 = the game's roads
N_WORLDS = 10
ROAD_MAX_ROWS = 500


class Source:
    """The user's SkyRoads files, from a zip or a directory."""

    def __init__(self, path):
        self.path = path
        self.names = {}
        if os.path.isdir(path):
            for n in os.listdir(path):
                self.names[n.upper()] = os.path.join(path, n)
            self.zip = None
        elif zipfile.is_zipfile(path):
            self.zip = zipfile.ZipFile(path)
            for n in self.zip.namelist():
                self.names[os.path.basename(n).upper()] = n
        else:
            raise SystemExit(f"{path}: neither a folder nor a zip")

    def has(self, name):
        return name.upper() in self.names

    def read(self, name):
        key = name.upper()
        if key not in self.names:
            raise SystemExit(f"{name} not found in {self.path}")
        if self.zip:
            return self.zip.read(self.names[key])
        with open(self.names[key], 'rb') as f:
            return f.read()


def walk_gfx(data):
    """Yield ('cmap', [(r6,g6,b6)...]) and ('pict', screen_ofs, w, h, pixels)
    from a CMAP/PICT container, pixels NOT yet rebased."""
    s = Stream(data, 0)
    while s.offset < len(data):
        tag = s.read_bytes(4)
        if tag == b'CMAP':
            count = s.read_byte()
            rgb = s.read_bytes(3 * count)
            s.read_bytes(2 * count)                  # EGA palette, unused
            yield ('cmap', [tuple(rgb[i * 3:i * 3 + 3]) for i in range(count)])
        elif tag == b'PICT':
            ofs = s.read_u16()
            h = s.read_u16()
            w = s.read_u16()
            yield ('pict', ofs, w, h, decompress(s, w * h))
        else:
            return


def load_gfx(src, name):
    """Returns (cmaps, picts): lists in file order."""
    cmaps, picts = [], []
    for c in walk_gfx(src.read(name)):
        if c[0] == 'cmap':
            cmaps.append(c[1])
        else:
            picts.append(c[1:])
    return cmaps, picts


def rebase(pixels, base):
    """What the original loader does: every non-zero pixel moves up by the
    file's palette base, zero stays zero (transparent)."""
    return bytes((p + base) & 0xFF if p else 0 for p in pixels)


def load_roads(src):
    """List of dicts: gravity, word2, word3, palette (72 x rgb6), cells
    (list of 7-tuples of u16)."""
    data = src.read('ROADS.LZS')
    first = struct.unpack_from('<H', data, 0)[0]
    n = first // 4
    roads = []
    for i in range(n):
        off, raw = struct.unpack_from('<HH', data, i * 4)
        s = Stream(data, off)
        gravity, w2, w3 = s.read_u16(), s.read_u16(), s.read_u16()
        pal = s.read_bytes(216)
        grid = decompress(s, raw)
        rows = raw // 14
        cells = [struct.unpack_from('<7H', grid, r * 14) for r in range(rows)]
        roads.append(dict(gravity=gravity, word2=w2, word3=w3,
                          palette=[tuple(pal[k * 3:k * 3 + 3]) for k in range(72)],
                          cells=cells))
    return roads


def load_trekdat(src):
    """The eight road-phase span objects, after the original's second-stage
    in-place expansion (EXE fn_3a7a): returns a list of bytes objects in the
    EXPANDED layout - 0x270-byte directory of u16 LE record offsets, then
    1040 records {u8 colour, u16 LE anchor, (u8 off, u8 len, u8 pad)*, 0xFF}."""
    data = src.read('TREKDAT.LZS')
    s = Stream(data, 0)
    objs = []
    while s.offset + 4 <= len(data):
        raw = s.read_u16()
        comp = s.read_u16()
        if raw == 0 or comp > raw:
            break
        packed = decompress(s, comp)
        objs.append(expand_trek(packed, raw))
        if s.offset >= len(data):
            break
    return objs


def expand_trek(packed, raw_size):
    # The object as loaded: raw_size bytes, the LZS output in its tail, the
    # word at 0 = where that tail starts.  The expansion reads from the tail
    # and writes from the head; here the tail is simply `packed`.
    src = packed
    out = bytearray()
    si = 0
    out += src[si:si + 0x270]
    si += 0x270
    for _ in range(0x410):
        if si >= len(src):
            break
        out += src[si:si + 3]                        # colour, anchor lo, hi
        si += 3
        while True:
            b = src[si]
            si += 1
            out.append(b)
            if b == 0xFF:
                break
            out.append(src[si])                      # length
            si += 1
            out.append(0)                            # pad
    if len(out) != raw_size:
        raise SystemExit(f"TREKDAT expansion: {len(out)} bytes, expected {raw_size}")
    return bytes(out)


#: File offset of the data segment in SKYROADS.EXE: the 0x200-byte MZ header
#: plus 0x66E0, the offset skyroads-port's notes give into the load image.
EXE_DS = 0x68E0


def exe_tables(src):
    """Tables of the original's data segment, read from the user's EXE.
    Anchored by two signatures whose values the reimplemented physics already
    fixes (play.c / render.c): if they do not match, this is another build of
    the EXE and nothing else read from it can be trusted."""
    exe = src.read('SKYROADS.EXE')
    pan = struct.unpack_from('<7h', exe, EXE_DS + 0x38)
    top = struct.unpack_from('<6H', exe, EXE_DS + 0xDE)
    if pan != (-1, -1, -1, 0, 1, 2, 4) or top != (0x2800, 0x3200, 0x3200, 0x3200, 0x3C00, 0x3C00):
        raise SystemExit("SKYROADS.EXE: data segment signatures not found - unknown EXE version")
    return {
        # ds:0x322: 74 x {VGA left, VGA right, EGA left, EGA right} fill bytes
        'quad': exe[EXE_DS + 0x322:EXE_DS + 0x322 + 74 * 4],
        # ds:0x65e: 5 shadow stencils, 29 columns x 9 rows, column-major
        'shadow': exe[EXE_DS + 0x65E:EXE_DS + 0x65E + 5 * 261],
        # ds:0x44a: dashboard cowl half-width per viewport row (138 u16)
        'cowl': struct.unpack_from('<138H', exe, EXE_DS + 0x44A),
    }


def vga_rgb8(c):
    """6-bit VGA DAC -> 8-bit, the usual (c<<2)|(c>>4)."""
    return tuple((v << 2) | (v >> 4) for v in c)
