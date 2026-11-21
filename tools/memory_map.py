#!/usr/bin/env python3
"""The ONE place every address of the project is decided.

    python tools/memory_map.py

Generates, and nobody edits by hand:
  include/map.inc     the EQUs RMAC assembles against
  csrc/mem_map.h      the same numbers for the C blob
  include/jag_c.ld    the C blob's linker script
  build/layout.txt    a readable dump

DRAM regions are laid out in order, each aligned to its own need; the HAL cells
are aligned to THEIR size (a long on a multiple of 4), because the GPU and the
DSP ignore the two low address bits on a long access and would silently read
four bytes earlier (Keen Dreams lost its audio to exactly that).

Cartridge regions follow the REAL sizes of the converted blobs in cart/, so this
runs after tools/sr2jag.py.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CART_DIR = os.path.join(REPO, 'cart')

DRAM_SIZE = 0x200000
ROM_BASE = 0x800000
ROM_CODE_OFS = 0x2000            # the $F6 fastboot header jumps here
ROM_CODE_SIZE = 0x10000
ROM_CBLOB_SIZE = 0x20000

# Switches that more than one party reads (RMAC, linker script, C).
SWITCHES = {
    'C_BLOB': 0,                 # M0 has no C blob: one suspect per fault
}

# --- the game screen inside the 320x240 progressive frame ------------------
SCR_W = 320
SCR_H = 240                      # the video window, as Keen Dreams (verified)
GAME_H = 200                     # SkyRoads' mode 13h screen
GAME_Y = (SCR_H - GAME_H) // 2   # centred: 20 black rows above and below
VIEW_H = 138                     # the 3D viewport (rows 0..137)
DASH_Y = 129                     # the dashboard picture starts here
DASH_H = 71

WORLD_BYTES = 320 * 138
DASH_BYTES = 320 * 71
CAR_BYTES = 32 * 24
CAR_FRAMES = 77

# --- HAL cells: (name, size). Aligned to their size. -------------------------
HAL_CELLS = [
    ('IRQ_TICK', 4),             # video interrupts since boot
    ('BOOT_MARK', 4),            # last boot stage reached
    ('BLIT_STALL', 4),
    ('VID_NTSC', 2),
    ('VDB_VAL', 2),
    ('VDE_VAL', 2),
    ('VI_VAL', 2),
    ('TICK_HZ', 2),
    ('CUR_ROAD', 2),             # which road's CLUT/world is shown
    ('CUR_WORLD', 2),
    ('PAD_STATE', 4),
    ('PAD_PREV', 4),
    ('PAD_EDGE', 4),
]

# --- OP objects, in LIST ORDER = priority (whoever is drawn last wins) -------
OBJECTS = [
    'BACKDROP',                  # the world picture, opaque, rows 0..137
    'DASH',                      # the dashboard, transparent, rows 129..199
]

# --- DRAM regions: (name, size, align). Laid out in this order. -------------
def dram_regions():
    n_obj = len(OBJECTS)
    return [
        ('VECTORS', 0x400, 8),
        ('HAL', 0x400, 8),
        # The list the OP reads: two branches, the objects, the final STOP.
        ('OP_LIST', 16 + 32 * n_obj + 16, 32),
        # The truth the video interrupt re-arms the list from (the OP consumes
        # HEIGHT and DATA as it draws).
        ('OBJ_SHADOW', 32 * n_obj, 32),
        ('STACK', 0x2000, 8),
        ('DCODE', ROM_CODE_SIZE, 8),
        ('BACKDROP_BUF', WORLD_BYTES, 64),
        ('DASH_BUF', DASH_BYTES, 64),
        ('CARS_BUF', CAR_BYTES * CAR_FRAMES, 64),
    ]


# --- cartridge regions: (name, file) in order; sizes from cart/ ---------------
CART_BLOBS = [
    ('ROM_CLUT', 'game_clut.bin'),
    ('ROM_WORLDS', 'worlds.bin'),
    ('ROM_DASH', 'dash.bin'),
    ('ROM_CARS', 'cars.bin'),
    ('ROM_ROADS', 'roads.bin'),
    ('ROM_TREK', 'trek.bin'),
    ('ROM_DEMO', 'demo.bin'),
]


def align(v, a):
    return (v + a - 1) // a * a


def dram_layout():
    out, addr = [], 0
    for name, size, al in dram_regions():
        addr = align(addr, al)
        out.append((name, addr, size))
        addr += size
    if addr > DRAM_SIZE:
        raise SystemExit(f"DRAM overflow: ${addr:X} > ${DRAM_SIZE:X}")
    return out, addr


def hal_layout(base, size):
    out, addr = [], base
    for name, sz in sorted(HAL_CELLS, key=lambda c: -c[1]):
        addr = align(addr, sz)
        out.append((name, addr, sz))
        addr += sz
    if addr > base + size:
        raise SystemExit("HAL cells overflow their region")
    for name, a, sz in out:
        assert a % sz == 0, name
    return out


def cart_layout():
    """[(sym, cpu_addr, file_offset, size, file)], ROM_CODE first."""
    out = [('ROM_CODE', ROM_BASE + ROM_CODE_OFS, ROM_CODE_OFS, ROM_CODE_SIZE, None)]
    ofs = ROM_CODE_OFS + ROM_CODE_SIZE
    if SWITCHES['C_BLOB']:
        out.append(('ROM_CBLOB', ROM_BASE + ofs, ofs, ROM_CBLOB_SIZE, None))
        ofs += ROM_CBLOB_SIZE
    for sym, fn in CART_BLOBS:
        path = os.path.join(CART_DIR, fn)
        if not os.path.isfile(path):
            raise SystemExit(f"cart/{fn} missing: run tools/sr2jag.py first")
        size = align(os.path.getsize(path), 8)
        ofs = align(ofs, 8)
        out.append((sym, ROM_BASE + ofs, ofs, size, fn))
        ofs += size
    return out, ofs


def cart_size():
    _, end = cart_layout()
    for mb in (2, 4):
        if end <= mb << 20:
            return mb << 20
    raise SystemExit(f"cartridge content {end:,} bytes exceeds 4 MB")


def main():
    dram, dram_end = dram_layout()
    regs = dict((n, (a, s)) for n, a, s in dram)
    hal = hal_layout(*regs['HAL'])
    cart, cart_end = cart_layout()

    eq = []           # (name, value, comment)
    for n, a, s in dram:
        eq.append((n, a, f'DRAM, {s} bytes'))
        eq.append((n + '_SIZE', s, None))
    for n, a, s in hal:
        eq.append((n, a, f'HAL cell, {s} bytes'))
    for sym, addr, ofs, size, fn in cart:
        eq.append((sym, addr, f'cartridge, {size} bytes' + (f' <- cart/{fn}' if fn else '')))
        eq.append((sym + '_SIZE', size, None))
    op = regs['OP_LIST'][0]
    # A branch whose condition is false falls through to the NEXT PHRASE, not
    # to its link: the two branches and the first object must be adjacent.
    # (M0's first hardware round: BR_VDB at +16 left a filler STOP at +8, the
    # OP stopped there on every line and the screen showed BG only.)
    eq += [
        ('OP_BR_VDE', op, 'branch: below the window -> STOP'),
        ('OP_BR_VDB', op + 8, 'branch: above the window -> STOP (falls through from BR_VDE)'),
        ('OP_OBJS', op + 16, 'first object, falls through from BR_VDB (32-byte cells)'),
        ('OP_STOP', op + 16 + 32 * len(OBJECTS), 'the final STOP'),
        ('OBJ_STRIDE', 32, None),
        ('OBJ_N', len(OBJECTS), None),
        ('STACK_TOP', regs['STACK'][0] + regs['STACK'][1], None),
    ]
    for i, o in enumerate(OBJECTS):
        eq.append(('OBJ_' + o, i, 'index in the list'))
    eq += [(k, v, 'switch') for k, v in SWITCHES.items()]
    eq += [('SCR_W', SCR_W, None), ('SCR_H', SCR_H, None), ('GAME_H', GAME_H, None),
           ('GAME_Y', GAME_Y, None), ('VIEW_H', VIEW_H, None), ('DASH_Y', DASH_Y, None),
           ('DASH_H', DASH_H, None), ('WORLD_BYTES', WORLD_BYTES, None),
           ('WORLD_STRIDE', align(WORLD_BYTES, 8), None), ('DASH_BYTES', DASH_BYTES, None),
           ('CAR_BYTES', CAR_BYTES, None), ('CAR_FRAMES', CAR_FRAMES, None),
           ('CLUT_BYTES', 512, None), ('N_ROADS', 31, None), ('N_WORLDS', 10, None)]

    os.makedirs(os.path.join(REPO, 'include'), exist_ok=True)
    os.makedirs(os.path.join(REPO, 'csrc'), exist_ok=True)
    os.makedirs(os.path.join(REPO, 'build'), exist_ok=True)

    hdr = 'GENERATED BY tools/memory_map.py - DO NOT EDIT BY HAND.'
    with open(os.path.join(REPO, 'include', 'map.inc'), 'w', newline='\n') as f:
        f.write(f'; {hdr}\n\n')
        for n, v, c in eq:
            if c:
                f.write(f'; {c}\n')
            f.write(f'{n:<16} EQU     ${v:X}\n')
    with open(os.path.join(REPO, 'csrc', 'mem_map.h'), 'w', newline='\n') as f:
        f.write(f'/* {hdr} */\n#ifndef MEM_MAP_H\n#define MEM_MAP_H\n\n')
        for n, v, c in eq:
            f.write(f'#define {n:<16} 0x{v:X}u\n')
        f.write('\n#endif\n')

    with open(os.path.join(REPO, 'build', 'layout.txt'), 'w') as f:
        f.write('DRAM\n')
        for n, a, s in dram:
            f.write(f'  {n:<14} ${a:06X} .. ${a + s:06X}  {s:>8,}\n')
        f.write(f'  (free from ${dram_end:06X}: {DRAM_SIZE - dram_end:,} bytes)\n\nHAL\n')
        for n, a, s in hal:
            f.write(f'  {n:<14} ${a:06X}  {s}\n')
        f.write('\nCARTRIDGE\n')
        for sym, addr, ofs, size, fn in cart:
            f.write(f'  {sym:<12} ${addr:06X}  {size:>9,}  {fn or ""}\n')
        f.write(f'  (end ${ROM_BASE + cart_end:06X}, cart {cart_size() >> 20} MB)\n')
    print(f"memory_map: DRAM used to ${dram_end:06X}, cartridge to ${ROM_BASE + cart_end:06X}"
          f" ({cart_size() >> 20} MB)")


if __name__ == '__main__':
    main()
