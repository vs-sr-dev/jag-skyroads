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
  trek.bin       index of 8 x 8 bytes, then the 8 expanded span objects
                 (little-endian inside, as the original has them).
  demo.bin       DEMO.REC verbatim.
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

    write('demo.bin', pad8(src.read('DEMO.REC')))

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
