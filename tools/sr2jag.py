#!/usr/bin/env python3
"""SkyRoads data -> Jaguar cartridge blobs.

    python tools/sr2jag.py [SOURCE]

SOURCE is the freeware distribution's zip or a folder holding its files
(default: skyroads.zip, then gamedata/, in the repository root).  The blobs go
to cart/, the previews to build/preview/.  Neither is ever committed: they ARE
Bluemoon's data, converted.

Every blob is big-endian (the 68000's order) and every pixel array starts on a
phrase (8 bytes), because the Object Processor reads them as they are.

Palettes go out as Jaguar RGB16, which is NOT RGB565:
    bits 15-11 red (5), bits 10-6 BLUE (5), bits 5-0 GREEN (6)
The VGA DAC is 6 bits per channel, so green keeps all of its precision and red
and blue lose their lowest bit.

Blobs:
  game_clut.bin  31 full 256-entry CLUTs, one per road: road palette (0..71),
                 cars (72..91), dashboard (92..141), the road's world
                 (142..255).  Composed HERE so the 68000 copies 512 bytes and
                 knows nothing about palette bases.
  worlds.bin     10 backdrops, 320x138, pixels already rebased (+142).
  dash.bin       the dashboard, 320x71, rebased (+92).
  cars.bin       77 ship/explosion frames, 32x24 row-major (the strip is
                 stored column-major, 24 rows x 30 columns per cell), +72.
  roads.bin      index of 31 x 16 bytes, then per road rows*7 u16 tile codes.
  trekg.bin      the 8 road-phase span objects re-laid for the GPU (see
                 trek_gpu below).
  tables.bin     the renderer's tables from SKYROADS.EXE's data segment.
  demo.bin       u32 length, then DEMO.REC verbatim.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import srdata as sd

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CART = os.path.join(REPO, 'cart')
PREVIEW = os.path.join(REPO, 'build', 'preview')

WORLD_W, WORLD_H = 320, 138
DASH_W, DASH_H, DASH_Y = 320, 71, 129
CAR_CELL_W, CAR_CELL_H = 24, 30          # as stored in the strip
CAR_W, CAR_H = 32, 24                    # as the OP reads it: 29 used + 3 pad
CAR_FRAMES = 77


def rgb16(c):
    r6, g6, b6 = c
    r5 = (r6 * 31 + 31) // 63
    b5 = (b6 * 31 + 31) // 63
    return (r5 << 11) | (b5 << 6) | g6


def rgb16_to_rgb8(v):
    r5, b5, g6 = v >> 11, (v >> 6) & 31, v & 63
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def world_of_road(entry):
    """game.c: the demo road (0) uses world 0, road n uses world (n-1)/3."""
    return 0 if entry == 0 else (entry - 1) // 3


def pad8(b):
    return b + bytes(-len(b) % 8)


def write(name, data):
    with open(os.path.join(CART, name), 'wb') as f:
        f.write(data)
    print(f"  {name:<14} {len(data):>8,} bytes")


def png(name, w, h, pixels, clut):
    from PIL import Image
    img = Image.new('RGB', (w, h))
    img.putdata([rgb16_to_rgb8(clut[p]) for p in pixels])
    img.save(os.path.join(PREVIEW, name))


# --- the road's span tables, re-laid for the GPU ------------------------------
#
# The original's records (renderer.md sec.4.3) are byte streams: colour,
# anchor (always x=160, one scanline per entry), then (offset, length, pad)
# per scanline.  The GPU reads external memory safely only as aligned LONGS,
# so every record becomes:
#     u32 row       first scanline's offset in a road layer (y_rel * 320)
#     u32 n         scanlines
#     n x u32       (x0 << 16) | length   for the LEFT half; the right half
#                   is its mirror around x=160: x = 320 - x0 - length
# The record CHAINS of the original (a composer draws "the next record" by
# calling the fill again) become consecutive indices, and the directory holds
# record indices instead of byte offsets.  The 68000 needs the colour too.
#
# trekg.bin:  8 x u32 object offsets, then per object
#     +0  u32 records   +4 u32 colours offset   +8 u32 span-block offsets offset
#     +12 u16 dir[13 rows][4 columns][6 kinds]  (record indices)
#     colours: u8 per record;  offsets: u32 per record, relative to the BLOB
#     start;  then the span blocks.
# All offsets are relative to the blob start so the 68000 adds one base.
LAYER_ROWS = 106          # viewport rows 32..137, the only ones roads reach
DIR_ENTRIES = 13 * 4 * 6


def parse_records(o):
    """{byte offset: (colour, y_rel, [(x0, len)...])}, in file order."""
    recs = {}
    si = 0x270
    for _ in range(0x410):
        start = si
        colour = o[si]
        anchor = o[si + 1] | (o[si + 2] << 8)
        si += 3
        if (anchor - 160) % 320:
            raise SystemExit(f"TREKDAT record @{start:#x}: anchor {anchor} not on x=160")
        y_rel = (anchor - 160) // 320
        spans = []
        while o[si] != 0xFF:
            off, ln = o[si], o[si + 1]
            si += 3
            x0 = 160 - off
            if x0 < 0 or x0 + ln > 320:
                raise SystemExit(f"TREKDAT record @{start:#x}: span off {off} len {ln} leaves the screen")
            spans.append((x0, ln))
        si += 1
        if spans and (y_rel < 0 or y_rel + len(spans) > LAYER_ROWS):
            raise SystemExit(f"TREKDAT record @{start:#x}: rows {y_rel}..{y_rel + len(spans)} outside the layer")
        recs[start] = (colour, y_rel, spans)
    return recs


def trek_gpu(objs):
    blob = bytearray(4 * len(objs))
    for k, o in enumerate(objs):
        recs = parse_records(o)
        order = list(recs)                                  # file order = chain order
        index = {ofs: i for i, ofs in enumerate(order)}
        dir_ofs = struct.unpack_from(f'<{DIR_ENTRIES}H', o, 0)
        dir_idx = []
        for d in dir_ofs:
            if d not in index:
                raise SystemExit(f"TREKDAT object {k}: directory entry {d:#x} is not a record")
            dir_idx.append(index[d])
        base = len(blob)
        struct.pack_into('>I', blob, 4 * k, base)
        n = len(order)
        hdr = 12 + 2 * DIR_ENTRIES
        col_ofs = hdr
        off_ofs = col_ofs + n + (-(col_ofs + n) % 4)
        spans_at = off_ofs + 4 * n
        obj = bytearray(spans_at)
        struct.pack_into('>III', obj, 0, n, col_ofs, off_ofs)
        struct.pack_into(f'>{DIR_ENTRIES}H', obj, 12, *dir_idx)
        body = bytearray()
        for i, ofs in enumerate(order):
            colour, y_rel, spans = recs[ofs]
            obj[col_ofs + i] = colour
            struct.pack_into('>I', obj, off_ofs + 4 * i, base + spans_at + len(body))
            body += struct.pack('>II', max(y_rel, 0) * 320, len(spans))
            for x0, ln in spans:
                body += struct.pack('>I', (x0 << 16) | ln)
        blob += obj + body
        blob += bytes(-len(blob) % 8)
    return bytes(blob)


# tables.bin: the EXE's data-segment tables the renderer needs (offsets are
# fixed in csrc/shared.h: TAB_QUAD, TAB_SHADOW, TAB_COWL).
def tables_blob(t):
    b = bytearray(t['quad'])                         # 296 bytes at 0
    b += bytes(-len(b) % 8)
    b += t['shadow']                                 # 1305 bytes at 296
    b += bytes(-len(b) % 8)
    b += struct.pack('>138H', *t['cowl'])            # at 1608
    b += bytes(-len(b) % 8)
    assert len(t['quad']) == 296 and len(t['shadow']) == 1305
    return bytes(b)


def find_source(argv):
    if len(argv) > 1:
        return argv[1]
    for cand in ('skyroads.zip', 'gamedata'):
        p = os.path.join(REPO, cand)
        if os.path.exists(p):
            return p
    raise SystemExit("no SkyRoads data: pass the zip or folder as argument")


def main(argv):
    src = sd.Source(find_source(argv))
    os.makedirs(CART, exist_ok=True)
    os.makedirs(PREVIEW, exist_ok=True)
    print(f"sr2jag: reading {src.path}")

    # --- the fixed parts of the game palette -------------------------------
    (cars_pal,), cars_picts = sd.load_gfx(src, 'CARS.LZS')
    (dash_pal,), dash_picts = sd.load_gfx(src, 'DASHBRD.LZS')
    roads = sd.load_roads(src)
    if len(roads) != sd.N_ROADS:
        raise SystemExit(f"ROADS.LZS: {len(roads)} roads, expected {sd.N_ROADS}")

    worlds = []
    for w in range(sd.N_WORLDS):
        (pal,), picts = sd.load_gfx(src, f'WORLD{w}.LZS')
        ofs, pw, ph, px = picts[0]
        if (ofs, pw, ph) != (0, WORLD_W, WORLD_H):
            raise SystemExit(f"WORLD{w}: unexpected picture {pw}x{ph}@{ofs}")
        worlds.append((pal, sd.rebase(px, sd.BASE_WORLD)))

    # --- CLUTs: one per road -----------------------------------------------
    cluts = []
    for e, road in enumerate(roads):
        pal = [(0, 0, 0)] * 256
        for i, c in enumerate(road['palette']):
            pal[sd.BASE_ROAD + i] = c
        for i, c in enumerate(cars_pal):
            pal[sd.BASE_CARS + i] = c
        for i, c in enumerate(dash_pal):
            pal[sd.BASE_DASH + i] = c
        for i, c in enumerate(worlds[world_of_road(e)][0]):
            if sd.BASE_WORLD + i < 256:
                pal[sd.BASE_WORLD + i] = c
        cluts.append([rgb16(c) for c in pal])
    write('game_clut.bin', b''.join(struct.pack('>256H', *c) for c in cluts))

    # --- backdrops ----------------------------------------------------------
    write('worlds.bin', b''.join(pad8(px) for _, px in worlds))

    # --- dashboard: the VGA picture (the second PICT is the EGA one) --------
    ofs, pw, ph, px = dash_picts[0]
    if (ofs, pw, ph) != (DASH_Y * 320, DASH_W, DASH_H):
        raise SystemExit(f"DASHBRD: unexpected picture {pw}x{ph}@{ofs}")
    dash = sd.rebase(px, sd.BASE_DASH)
    write('dash.bin', pad8(dash))

    # --- the ship strip, transposed ------------------------------------------
    ofs, pw, ph, px = cars_picts[0]
    if pw != CAR_CELL_W or ph != CAR_CELL_H * CAR_FRAMES:
        raise SystemExit(f"CARS: unexpected strip {pw}x{ph}")
    strip = sd.rebase(px, sd.BASE_CARS)
    cars = bytearray()
    cell = CAR_CELL_W * CAR_CELL_H
    for f in range(CAR_FRAMES):
        c = strip[f * cell:(f + 1) * cell]
        # render.c draw_ship: px = cell[j*24 + i], screen column j (0..28),
        # screen row i (0..23).
        for i in range(CAR_H):
            for j in range(CAR_W):
                cars.append(c[j * 24 + i] if j < 29 else 0)
    write('cars.bin', bytes(cars))

    # --- roads ----------------------------------------------------------------
    index = bytearray()
    body = bytearray()
    base = 16 * len(roads)
    for road in roads:
        rows = len(road['cells'])
        if rows > sd.ROAD_MAX_ROWS:
            raise SystemExit(f"a road has {rows} rows (max {sd.ROAD_MAX_ROWS})")
        index += struct.pack('>IHHHHI', base + len(body), rows, road['gravity'],
                             road['word2'], road['word3'], 0)
        for row in road['cells']:
            body += struct.pack('>7H', *row)
        body += bytes(-len(body) % 4)
    write('roads.bin', bytes(index + body))

    # --- trekdat ----------------------------------------------------------------
    objs = sd.load_trekdat(src)
    if len(objs) != 8:
        raise SystemExit(f"TREKDAT: {len(objs)} objects, expected 8")
    index = bytearray()
    body = bytearray()
    for o in objs:
        index += struct.pack('>II', 64 + len(body), len(o))
        body += pad8(o)
    write('trek.bin', bytes(index + body))

    write('trekg.bin', trek_gpu(objs))
    write('tables.bin', tables_blob(sd.exe_tables(src)))

    demo = src.read('DEMO.REC')
    write('demo.bin', pad8(struct.pack('>I', len(demo)) + demo))

    # --- previews: what the Jaguar should show, from the blobs ------------------
    clut = cluts[1]
    png('world0.png', WORLD_W, WORLD_H, worlds[0][1], clut)
    png('dash.png', DASH_W, DASH_H, dash, clut)
    png('cars.png', CAR_W, CAR_H * CAR_FRAMES, bytes(cars), clut)
    frame = bytearray(320 * 200)
    frame[:WORLD_W * WORLD_H] = worlds[0][1]
    for y in range(DASH_H):
        for x in range(DASH_W):
            p = dash[y * DASH_W + x]
            if p:
                frame[(DASH_Y + y) * 320 + x] = p
            elif DASH_Y + y >= WORLD_H:
                frame[(DASH_Y + y) * 320 + x] = 0
    png('m0_expected.png', 320, 200, bytes(frame), clut)
    print(f"sr2jag: previews in {os.path.relpath(PREVIEW, REPO)}")


if __name__ == '__main__':
    main(sys.argv)
