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
  game_pal.bin   31 full 256-entry palettes, one per road, 6-bit VGA r, g, b:
                 road palette (0..71), cars (72..91), dashboard (92..141), the
                 road's world (142..255).  Composed HERE so the 68000 knows
                 nothing about palette bases; it fades them as the original
                 (fn_4315) and converts them to Jaguar RGB16 (csrc/front.c).
  worlds.bin     10 backdrops, 320x138, pixels already rebased (+142).
  dash.bin       the dashboard, 320x71, rebased (+92).
  cars.bin       77 ship/explosion frames, 32x24 row-major (the strip is
                 stored column-major, 24 rows x 30 columns per cell), +72.
  roads.bin      index of 31 x 16 bytes, then per road rows*7 u16 tile codes.
  trekg.bin      the 8 road-phase span objects re-laid for the GPU (see
                 trek_gpu below).
  tables.bin     the renderer's tables from SKYROADS.EXE's data segment.
  demo.bin       u32 length, then DEMO.REC verbatim.
  muzax.bin      the songs as OPL2 register streams (tools/muzax.py), which
                 Jerry plays through its synthesised OPL2 (src/dsp_audio.s).
  sfx.bin        SFX.SND's five effects and INTRO.SND, resampled to Jerry's
                 rate (tools/opl2.py FS), signed 8-bit (see sfx_blob).
  front.bin      the intro, the menus and the road map: pictures rebased as
                 the original's loader does, colour maps (see front_blob).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import srdata as sd
from lzs import Stream, decompress
import muzax
import opl2

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
# record indices instead of byte offsets.
#
# trekg.bin:  8 x u32 object offsets, then per object, LONGS ONLY, because
# the GPU runs the composers too (src/gpu_road.s):
#     +0  u32 records
#     +4  u32 offset of the record table, from the object
#     +8  u32 dir[13 rows][4 columns][6 kinds]   (record indices)
#     record table: u32 per record = colour << 24 | span block offset from the
#         BLOB start.  A colour >= 74 is stored as 0: the composers index the
#         74-entry colour-quad table with "colour < 74 ? colour : 0" (render.c),
#         so both give quad 0 and the GPU needs no clamp.
#     then the span blocks.
# All span offsets are relative to the blob start so the reader adds one base.
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
        rec_ofs = 8 + 4 * DIR_ENTRIES
        spans_at = rec_ofs + 4 * n
        obj = bytearray(spans_at)
        struct.pack_into('>II', obj, 0, n, rec_ofs)
        struct.pack_into(f'>{DIR_ENTRIES}I', obj, 8, *dir_idx)
        body = bytearray()
        for i, ofs in enumerate(order):
            colour, y_rel, spans = recs[ofs]
            at = base + spans_at + len(body)
            if at >> 24:
                raise SystemExit(f"TREKDAT object {k}: span block at {at:#x} does not fit 24 bits")
            struct.pack_into('>I', obj, rec_ofs + 4 * i, ((colour if colour < 74 else 0) << 24) | at)
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


# hud.bin: the dashboard's moving parts (csrc/hud.c).
#   56 x u32 offsets: 34 speed segments, 10 oxygen, 10 fuel, the digits, the
#   jump-o-master lamps;  a segment record is u16 x, u16 y, u16 w, u16 h, then
#   w*h cells (0 = not part of it, 1 / 2 = which colour), padded to 2.
HUD_SPEED, HUD_OXY, HUD_FUL = 34, 10, 10


def hud_blob(src, tabs):
    segs = (sd.load_segments(src, 'SPEED.DAT', HUD_SPEED, 2 * HUD_SPEED)
            + sd.load_segments(src, 'OXY_DISP.DAT', HUD_OXY, 2 * HUD_OXY)
            + sd.load_segments(src, 'FUL_DISP.DAT', HUD_FUL, 2 * HUD_FUL))
    n = len(segs) + 2
    blob = bytearray(4 * n)
    for i, (x, y, w, h, cells) in enumerate(segs):
        if not (0 <= x and x + w <= 320 and 129 <= y and y + h <= 200):
            raise SystemExit(f"HUD segment {i} at {x},{y} {w}x{h} is outside the dashboard")
        struct.pack_into('>I', blob, 4 * i, len(blob))
        blob += struct.pack('>HHHH', x, y, w, h) + cells
        blob += bytes(len(blob) % 2)
    blob += bytes(-len(blob) % 4)
    struct.pack_into('>I', blob, 4 * len(segs), len(blob))
    blob += tabs['digits']
    struct.pack_into('>I', blob, 4 * (len(segs) + 1), len(blob))
    blob += tabs['aplight']
    return bytes(blob + bytes(-len(blob) % 8))


# Each effect's level, against the DSP's fixed gain (src/dsp_audio.s: an
# effect sample << 8, twice skyroads-port's << 7).  The landing and the other
# small effects were too quiet under the music on the Jaguar (user, session
# 4): +6 dB for them; the explosion (rms 86 against 25-47) and INTRO.SND keep
# the port's level.
SFX_GAIN = [0.5, 1.0, 1.0, 1.0, 1.0, 0.5]


def resample(pcm, rate_in, rate_out, taps=16, gain=1.0):
    """Unsigned 8-bit PCM -> signed 8-bit at rate_out, band-limited.

    A windowed sinc (Blackman, `taps` zero crossings a side) with its cutoff
    at 0.45 of the LOWER rate: SFX.SND's 50 kHz effect folds into Jerry's
    16 kHz otherwise.
    """
    import numpy as np
    x = np.frombuffer(pcm, dtype=np.uint8).astype(np.float64) - 128.0
    n_out = int(len(x) * rate_out / rate_in)
    fc = 0.45 * min(rate_in, rate_out) / rate_in          # cycles per input sample
    t = np.arange(n_out) * (rate_in / rate_out)
    base = np.floor(t).astype(np.int64)
    acc = np.zeros(n_out)
    span = int(np.ceil(taps / (2 * fc)))
    for k in range(-span, span + 1):
        idx = base + k
        d = t - idx
        w = 2 * fc * np.sinc(2 * fc * d)
        win = 0.42 + 0.5 * np.cos(np.pi * d / (span + 1)) + 0.08 * np.cos(2 * np.pi * d / (span + 1))
        ok = (idx >= 0) & (idx < len(x))
        acc += np.where(ok, x[np.clip(idx, 0, len(x) - 1)] * w * win, 0.0)
    return np.clip(np.round(acc * gain), -128, 127).astype(np.int8).tobytes()


def sfx_blob(src):
    """SFX.SND's records and INTRO.SND, at Jerry's rate.

        u32 n                   6: SFX.SND 0..4, INTRO.SND
        u32 offset, samples [n] from the blob's start
        samples                 signed 8-bit, each on a phrase, zero-padded
                                (the DSP reads a long per four samples and the
                                last one may run over by three bytes)

    The rates are the Sound Blaster's 1,000,000 / (256 - TC), TC being each
    record's first byte (EXE 0x443); INTRO.SND has no header and plays at
    TC $5A (0x476B).  Effect 2's TC $EC gives 50 kHz, beyond what an 8-bit
    single-cycle transfer could do on most cards; skyroads-port plays it at
    50 kHz and so do we (audio_misc.md 4.3: TBD on real hardware).
    """
    snd = src.read('SFX.SND')
    n = struct.unpack_from('<H', snd, 0)[0] // 2 - 1
    offs = struct.unpack_from('<%dH' % (n + 1), snd, 0)
    recs = []
    for i in range(n):
        rec = snd[offs[i]:offs[i + 1]]
        recs.append((rec[1:], 1000000 / (256 - rec[0])))
    recs.append((src.read('INTRO.SND'), 1000000 / (256 - 0x5A)))
    head = 4 + 8 * len(recs)
    table, data = bytearray(struct.pack('>I', len(recs))), bytearray()
    for (pcm, rate), gain in zip(recs, SFX_GAIN):
        out = resample(pcm, rate, opl2.FS, gain=gain)
        table += struct.pack('>II', head + len(data), len(out))
        data += pad8(out)
    return bytes(table + data)


# --- front.bin: the intro, the menus, the road map ----------------------------
#
# What the original loads for its screens (EXE fn_4575 intro, fn_4e36 main
# menu, fn_4c04 settings, fn_4dac help, fn_5164 road map), each picture
# REBASED by the colour base the LOADER passes (read_pict 0x4068: VGA adds it
# to every non-zero pixel), each colour map with the base it is loaded at.
# The order below is csrc/front.h's P_* / C_* enums: change both together
# (front.bin carries the counts and the C side checks them).
#
#   u32 'FRNT', u32 n_pics, u32 n_cmaps, u32 n_anim
#   n_pics  x { u16 screen_ofs, u16 w, u16 h, u16 0, u32 pixels }   rows of w
#   n_cmaps x { u16 base, u16 count, u32 rgb }    count x (r, g, b), 6 bits
#   n_anim  x { u16 frame, u16 picture }          ANIM.LZS's records in order
# Offsets from the blob's start; pixels start on a phrase.
FRONT_MAGIC = 0x46524E54
INTRO_SCENES = 7


def front_blob(src):
    pics, cmaps, anim = [], [], []

    def pict(p, base):
        ofs, w, h, px = p
        pics.append((ofs, w, h, sd.rebase(px, base)))

    def cmap(c, base):
        if base + len(c) > 256:
            raise SystemExit(f"a colour map of {len(c)} at {base} runs past 255")
        cmaps.append((base, c))

    # INTRO.LZS in the order fn_4575 reads it: A @0, title @0, B @$32,
    # C @$32, logo @$32, then 7 x (E @$A0, G @$A0, scene @$A0)
    items = list(sd.walk_gfx(src.read('INTRO.LZS')))
    kinds = ''.join('c' if it[0] == 'cmap' else 'p' for it in items)
    # (the file holds an eighth triple the loop at 0x461D never reaches)
    if not kinds.startswith('cpccp' + 'ccp' * INTRO_SCENES):
        raise SystemExit(f"INTRO.LZS: unexpected chunk sequence {kinds}")
    cmap(items[0][1], 0)
    pict(items[1][1:], 0)
    cmap(items[2][1], 0x32)
    cmap(items[3][1], 0x32)
    pict(items[4][1:], 0x32)
    for k in range(INTRO_SCENES):
        e, g, p = items[5 + 3 * k:8 + 3 * k]
        cmap(e[1], 0xA0)
        cmap(g[1], 0xA0)
        pict(p[1:], 0xA0)
    # P order wanted: title, logo, scenes; C order: A, B, C, E0..6, G0..6.
    # Re-sort what was appended in file order.
    p_title, p_logo, p_scene = pics[0], pics[1], pics[2:]
    c_a, c_b, c_c = cmaps[0], cmaps[1], cmaps[2]
    c_e, c_g = cmaps[3::2], cmaps[4::2]
    pics[:] = [p_title, p_logo] + p_scene
    cmaps[:] = [c_a, c_b, c_c] + c_e + c_g

    # MAINMENU.LZS: colour map @$BE, three boxes @$BE (fn_4e36)
    (c,), boxes = sd.load_gfx(src, 'MAINMENU.LZS')
    if len(boxes) != 3:
        raise SystemExit("MAINMENU.LZS: expected 3 pictures")
    cmap(c, 0xBE)
    for p in boxes:
        pict(p, 0xBE)

    # SETMENU.LZS: map @$C8, background @$C8, map @$FA, 10 items @$FA (fn_4c04)
    items = list(sd.walk_gfx(src.read('SETMENU.LZS')))
    if ''.join('c' if it[0] == 'cmap' else 'p' for it in items) != 'cpc' + 'p' * 10:
        raise SystemExit("SETMENU.LZS: unexpected chunk sequence")
    cmap(items[0][1], 0xC8)
    pict(items[1][1:], 0xC8)
    cmap(items[2][1], 0xFA)
    for it in items[3:]:
        pict(it[1:], 0xFA)

    # HELPMENU.LZS: (map @$C8, page @$C8) x 3; fn_4e12 shows two at most
    items = list(sd.walk_gfx(src.read('HELPMENU.LZS')))
    if ''.join('c' if it[0] == 'cmap' else 'p' for it in items)[:4] != 'cpcp':
        raise SystemExit("HELPMENU.LZS: unexpected chunk sequence")
    for k in range(2):
        cmap(items[2 * k][1], 0xC8)
        pict(items[2 * k + 1][1:], 0xC8)

    # GOMENU.LZS: map @0, the map picture @0, map @$F0, the tick @$F0 (fn_5164)
    items = list(sd.walk_gfx(src.read('GOMENU.LZS')))
    if ''.join('c' if it[0] == 'cmap' else 'p' for it in items) != 'cpcp':
        raise SystemExit("GOMENU.LZS: unexpected chunk sequence")
    cmap(items[0][1], 0)
    pict(items[1][1:], 0)
    cmap(items[2][1], 0xF0)
    pict(items[3][1:], 0xF0)

    # ANIM.LZS (fn_4575 0x4629): "ANIM", u16 n_frames, CMAP @0, then per
    # frame u16 n_picts and the pictures (@0, blitted opaque)
    data = src.read('ANIM.LZS')
    s = Stream(data, 0)
    if s.read_bytes(4) != b'ANIM':
        raise SystemExit("ANIM.LZS: no ANIM tag")
    n_frames = s.read_u16()
    if s.read_bytes(4) != b'CMAP':
        raise SystemExit("ANIM.LZS: no colour map")
    count = s.read_byte()
    rgb = s.read_bytes(3 * count)
    s.read_bytes(2 * count)
    cmap([tuple(rgb[i * 3:i * 3 + 3]) for i in range(count)], 0)
    for f in range(n_frames):
        for _ in range(s.read_u16()):
            if s.read_bytes(4) != b'PICT':
                raise SystemExit("ANIM.LZS: a frame's record is not a PICT")
            ofs, h, w = s.read_u16(), s.read_u16(), s.read_u16()
            anim.append((f, len(pics)))
            pict((ofs, w, h, decompress(s, w * h)), 0)
    if len(anim) > 300:                          # the EXE's table (0x46A0)
        raise SystemExit(f"ANIM.LZS: {len(anim)} records, the original holds 300")

    for ofs, w, h, _ in pics:
        if ofs % 320 + w > 320 or ofs // 320 + h > 200:
            raise SystemExit(f"a picture {w}x{h} at {ofs} leaves the screen")

    head = 16 + 12 * len(pics) + 8 * len(cmaps) + 4 * len(anim)
    head = (head + 7) // 8 * 8
    table = bytearray(struct.pack('>IIII', FRONT_MAGIC, len(pics), len(cmaps), len(anim)))
    data = bytearray()
    for ofs, w, h, px in pics:
        table += struct.pack('>HHHHI', ofs, w, h, 0, head + len(data))
        data += pad8(px)
    rgb_at = []
    for base, c in cmaps:
        rgb_at.append(head + len(data))
        data += bytes(v for t in c for v in t)
        data += bytes(-len(data) % 2)
    for (base, c), at in zip(cmaps, rgb_at):
        table += struct.pack('>HHI', base, len(c), at)
    for f, p in anim:
        table += struct.pack('>HH', f, p)
    table += bytes(head - len(table))
    return bytes(table + data), pics, cmaps


def front_previews(pics, cmaps):
    """A few screens as the original composes them, to look at."""
    def screen(cm, pp):
        pal = [(0, 0, 0)] * 256
        for k in cm:
            base, c = cmaps[k]
            for i, v in enumerate(c):
                pal[base + i] = v
        fb = bytearray(320 * 200)
        for k, opaque in pp:
            ofs, w, h, px = pics[k]
            for y in range(h):
                for x in range(w):
                    p = px[y * w + x]
                    if p or opaque:
                        fb[ofs + y * 320 + x] = p
        return bytes(fb), [rgb16(c) for c in pal]
    # title + logo with the menu's palette (A, B, C); the road map; settings;
    # the first help page
    for name, cm, pp in (('title.png', (0, 1, 2), ((0, True), (1, False))),
                         ('gomenu.png', (22, 23), ((25, True), (26, False))),
                         ('setmenu.png', (18, 19), ((12, True), (13, False), (18, False))),
                         ('help0.png', (20,), ((23, True),))):
        fb, clut = screen(cm, pp)
        png(name, 320, 200, fb, clut)


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

    # --- palettes: one per road, as the VGA DAC holds them (6 bits a gun) ---
    # The 68000 fades them with the original's formula (fn_4315) and turns
    # them into Jaguar RGB16 on the way to the CLUT (csrc/front.c).
    pals6 = []
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
        pals6.append(bytes(v for c in pal for v in c))
    write('game_pal.bin', b''.join(pals6))

    # --- the intro and the menus --------------------------------------------
    front, front_pics, front_cmaps = front_blob(src)
    write('front.bin', front)

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
    write('trekg.bin', trek_gpu(objs))
    tabs = sd.exe_tables(src)
    write('tables.bin', tables_blob(tabs))
    write('hud.bin', hud_blob(src, tabs))

    demo = src.read('DEMO.REC')
    write('demo.bin', pad8(struct.pack('>I', len(demo)) + demo))

    # --- sound: the songs as OPL2 streams, the effects at Jerry's rate -------
    write('muzax.bin', pad8(muzax.build_blob(src.read('MUZAX.LZS'))))
    write('sfx.bin', sfx_blob(src))

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
    front_previews(front_pics, front_cmaps)
    print(f"sr2jag: previews in {os.path.relpath(PREVIEW, REPO)}")


if __name__ == '__main__':
    main(sys.argv)
