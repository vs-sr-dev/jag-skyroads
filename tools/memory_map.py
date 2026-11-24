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
import re
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
    'C_BLOB': 1,                 # the game logic, compiled C (M1)
}

# Jerry's I2S rate: Fs = clk / (64 (SCLK + 1)) (JaguarDocs hw/07 rule 1).
# The OPL2 synthesiser, its tables (tools/opl2.py) and the effects resampled
# at build time (tools/sr2jag.py) all follow this ONE number.
AUDIO_SCLK = 36

# The C blob's signature, returned by c_init: 'SKY1'.
C_MAGIC = 0x534B5931
CBSS_SIZE = 0x10000

# --- the game screen inside the 320x240 progressive frame ------------------
SCR_W = 320
SCR_H = 240                      # the video window, as Keen Dreams (verified)
GAME_H = 200                     # SkyRoads' mode 13h screen
GAME_Y = (SCR_H - GAME_H) // 2   # centred: 20 black rows above and below
VIEW_H = 138                     # the 3D viewport (rows 0..137)
DASH_Y = 129                     # the dashboard picture starts here
DASH_H = 71

GAME_BYTES = 320 * 200
WORLD_BYTES = 320 * 138
DASH_BYTES = 320 * 71
CAR_BYTES = 32 * 24
CAR_FRAMES = 77

# The GPU composers' input block, csrc/road.h CIN_* (checked in game_main.c):
# offsets the GPU adds to COMP_IN.
CIN = {'POS': 0, 'BASE': 4, 'QUAD0': 16, 'QUAD1': 16 + 4 * 74, 'WIN': 16 + 8 * 74}
CIN_ROWS = 11
COMP_IN_SIZE = 0x400
assert CIN['WIN'] + CIN_ROWS * 32 <= COMP_IN_SIZE

# The ship and shadow the GPU draws between the two lists, csrc/road.h SIN_*
# (road_ship_input; checked in game_main.c): offsets the GPU adds to SHIP_IN.
SIN = {'SRC': 0, 'ROW0': 4, 'LEFT': 8, 'MASK': 12, 'SROW0': 16, 'SLEFT': 20,
       'CLIP': 32, 'SCLIP': 32 + 4 * 24}
SIN_BYTES = SIN['SCLIP'] + 4 * 9
SHIP_IN_SIZE = 0x100
assert SIN_BYTES <= SHIP_IN_SIZE

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
    # The pad, read by RMAC (ReadPad) every video frame: RMAC only.  The C
    # side keeps its own previous state for edges.
    ('PAD_STATE', 2),            # game buttons, PAD_B_* bits
    ('PAD_KEYS', 2),             # keypad, KEY_B_* bits (instruments only)
    ('GAME_MODE', 2),            # C only: 0 attract demo, 1 playing
    # RMAC only: 1 if the video counters came up with hc[10] != VC & 1
    # (LockVi, src/main.s), so VI was moved to the other parity.
    ('VI_FLIP', 2),
    # 36.0036 Hz game time.  ONE WRITER EACH: the video interrupt advances
    # TICK_ACC and GAME_TICKS, the main loop advances GAME_DONE; the ticks
    # still to run are the difference.
    ('TICK_ACC', 4),             # ISR only
    ('TICK_DIV', 4),             # set at boot: VI rate in 1e-4 Hz
    ('GAME_TICKS', 4),           # ISR only
    ('GAME_DONE', 4),            # main loop only
    # The attract demo checked against the PC oracle (C writes, RMAC reads).
    ('DEMO_TICK', 4),            # ticks into the current run
    ('DEMO_BAD', 4),             # first mismatching tick + 1, 0 = all match
    ('DEMO_RUNS', 4),            # completed runs that matched to the end
    # The GPU road launch (csrc/game_main.c writes GPU_CMD, the GPU GPU_DONE;
    # the 68000 clears DONE only before a launch, when the GPU is stopped).
    # GPU_CMD = 0 asks for the composers first; the GPU then leaves the
    # second list's address there for the next launch (src/gpu_road.s).
    ('GPU_CMD', 4),
    ('GPU_DONE', 4),
    # The frame buffer of frame ROAD_SEQ (C writes it just before ROAD_SEQ;
    # RMAC's SetView reads both, on the same thread).
    ('ROAD_VIEW', 4),
    # Rendered frames.  ROAD_SEQ: C only (frames finished; frame k is drawn
    # into VIEW(k&1)).  ROAD_SHOWN: RMAC only (the frame on screen).
    ('ROAD_SEQ', 4),
    ('ROAD_SHOWN', 4),
    ('GPU_FAIL', 4),             # C only: launches whose DONE never came
    # Ticks the main loop dropped (backlog beyond MAX_CATCHUP): RMAC only.
    ('TICK_DROP', 4),
    # The palette fade (csrc/game_main.c fade_publish): C writes FADE_PAL and
    # then FADE_SEQ + 1; the video interrupt copies FADE_PAL into the CLUT in
    # the blanking and sets FADE_SHOWN = FADE_SEQ.  One writer each.
    ('FADE_SEQ', 4),
    ('FADE_SHOWN', 4),
    # The GameDrive's debug channel (src/gd.s): RMAC only, C reads.
    ('GD_BIOS', 4),              # the GD BIOS's base in GDB_BUF, 0 = none
    ('GD_SEND_FN', 4),           # GdSendC for C to call, 0 = nothing is sent
    ('GD_DROP', 4),              # lines dropped (nobody read the last one)
    ('GD_TIMEOUT', 4),           # bytes whose exchange ran out of patience
    ('GD_VER', 4),               # firmware (BCD) << 16 | ASIC
    # Jerry (src/dsp_audio.s).  The 68000 writes the stream to play and the
    # effect to start, each with its SEQ cell LAST (Keen Dreams' protocol,
    # one delivery per frame); the DSP writes the counters, every 64 samples.
    ('MUS_ADR', 4),              # 68000: the stream's start
    ('MUS_LOOP', 4),             # 68000: its loop point
    ('MUS_SEQ', 4),              # 68000: +1 = play the two above
    ('SND_ADR', 4),              # 68000: the effect's samples
    ('SND_LEN', 4),              # 68000: how many
    ('SND_SEQ', 4),              # 68000: +1 = play them
    ('SND_STARVE', 4),           # DSP: interrupts that found the ring empty
    ('SND_IRQ', 4),              # DSP: I2S interrupts served
    ('SND_DONE', 4),             # DSP: samples produced
    ('SND_SPIN', 4),             # DSP: idle-loop turns with the ring full
    # RMAC's ReadPad, for C to call while the front end waits (csrc/front.c):
    # it saves what it uses (d0-d4).
    ('PAD_FN', 4),
    # The cartridge EEPROM (src/eeprom.s, Atari's primitives), for C:
    # ee_read(word) and ee_write(word, data).
    ('EE_READ_FN', 4),
    ('EE_WRITE_FN', 4),
    ('GD_STATE', 2),             # GdInstall: 0 ok, else where it failed
    ('GD_PEND', 2),              # the micro's answer to the last line pending
]

# --- OP objects, in LIST ORDER = priority (whoever is drawn last wins) -------
OBJECTS = [
    # The whole 320x200 game screen, opaque: ONE frame buffer the GPU and the
    # 68000 compose (pristine screen, road, ship, shadow, gauges; csrc/road.h).
    # Stacking road layers and the ship as objects put three full-width
    # objects on a line, and that stopped the real Jaguar dead (M2, hardware).
    'GAME',
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
        # The C blob runs from DRAM: ColdBoot copies the whole image (code,
        # rodata, data) here; .bss lives apart in CBSS, which the DRAM clear
        # has already zeroed and the copy does not touch.
        ('DBLOB', ROM_CBLOB_SIZE, 8),
        ('CBSS', CBSS_SIZE, 8),
        # The road's pristine screen (csrc/hud.c hud_pristine), the GPU's
        # copy source at the start of every frame.
        ('PRISTINE', GAME_BYTES, 64),
        ('CARS_BUF', CAR_BYTES * CAR_FRAMES, 64),
        # The road's span blocks, which the GPU reads: DRAM, not cartridge.
        ('TREKG_BUF', cart_blob_size('trekg.bin'), 64),
        # The viewport frame buffers (csrc/road.h VIEW_BYTES, checked there).
        # THREE: the video interrupt arms the OP list from OBJ_SHADOW before
        # SetView points it at the new frame, so a swap shows only from the
        # NEXT interrupt.  With two buffers the next frame was drawn into the
        # one still on screen (road flickering, session 2, hardware).
        ('VIEW0', GAME_BYTES, 64),
        ('VIEW1', GAME_BYTES, 64),
        ('VIEW2', GAME_BYTES, 64),
        # The GPU command list: csrc/road.h ROAD_CMD_MAX longs, checked there.
        ('CMD_BUF', 0x2400, 8),
        # The CHECK instrument's own list (keypad 9, csrc/game_main.c).
        ('CMD_CHK', 0x2400, 8),
        # The instruments' numbers, 320 x 76, copied into frames by the
        # Blitter (csrc/game_main.c ovl_draw / ovl_blit).
        ('OVL_BUF', 320 * 76, 64),
        # The GPU composers' input (csrc/road.h CIN_*): position, colour
        # quads, the grid rows of the frame as longs.
        ('COMP_IN', COMP_IN_SIZE, 8),
        # The ship and shadow the GPU draws (csrc/road.h SIN_*), every frame.
        ('SHIP_IN', SHIP_IN_SIZE, 8),
        # The five shadow stencils as 9 row masks each (road_shadow_masks):
        # the cartridge's bytes are unaligned for the GPU.
        ('SHADOW_MASKS', 5 * 9 * 4, 8),
        # The CLUT the next video interrupt loads (fades; see FADE_SEQ).
        ('FADE_PAL', 0x200, 8),
        # The GameDrive's BIOS image, fetched at boot (src/gd.s GdInstall).
        ('GDB_BUF', 0x1000, 8),
        # The OPL2's slow tables (tools/opl2.py opl2_rom.inc: DINC, AINC,
        # MULT, KSL), copied here by InitDspAudio.  At an ABSOLUTE address
        # because the DSP cannot name a 68000 label: RMAC relocates a movei
        # of one wrongly (DCODE lands in the wrong half; seen in main.bin).
        ('OPL_TABS', 640, 8),
        # The front end's pictures that get drawn INTO (csrc/front.c): the
        # title the main menu composes its logo into, the road map the ticks
        # are drawn into.  The original reloads them from disk each time.
        ('TITLE_BUF', GAME_BYTES, 64),
        ('MENU_BG', GAME_BYTES, 64),
    ]


# --- cartridge regions: (name, file) in order; sizes from cart/ ---------------
CART_BLOBS = [
    ('ROM_PAL', 'game_pal.bin'),
    ('ROM_WORLDS', 'worlds.bin'),
    ('ROM_DASH', 'dash.bin'),
    ('ROM_CARS', 'cars.bin'),
    ('ROM_ROADS', 'roads.bin'),
    ('ROM_TREKG', 'trekg.bin'),
    ('ROM_TABLES', 'tables.bin'),
    ('ROM_HUD', 'hud.bin'),
    ('ROM_DEMO', 'demo.bin'),
    ('ROM_DEMOX', 'demo_expect.bin'),   # tools/oracle/demo_oracle.c
    ('ROM_MUZAX', 'muzax.bin'),         # tools/muzax.py
    ('ROM_SFX', 'sfx.bin'),             # tools/sr2jag.py sfx_blob
    ('ROM_FRONT', 'front.bin'),         # tools/sr2jag.py front_blob
]


def cart_blob_size(fn):
    path = os.path.join(CART_DIR, fn)
    if not os.path.isfile(path):
        raise SystemExit(f"cart/{fn} missing: run tools/sr2jag.py first")
    return align(os.path.getsize(path), 32)


# Atari's jaguar.inc: build.bat's JAGINC (the folder), else this machine's.
JAGUAR_INC = os.path.join(os.environ.get('JAGINC', r'D:\Homebrew\Jaguar\include'), 'jaguar.inc')
# Atari's names the C side needs, exported as JAG_<name>: read from
# jaguar.inc itself, so there is no second, hand-written copy of them.
JAG_NAMES = ['A1_BASE', 'A1_FLAGS', 'A1_CLIP', 'A1_PIXEL', 'A1_STEP',
             'A2_BASE', 'A2_FLAGS', 'A2_PIXEL', 'A2_STEP', 'B_CMD', 'B_COUNT',
             'SRCEN', 'UPDA1', 'UPDA2', 'LFU_REPLACE',
             'PITCH1', 'PIXEL8', 'WID320', 'XADDPHR']


def jaguar_inc():
    vals = {}
    for line in open(JAGUAR_INC, errors='replace'):
        parts = line.split(';')[0].split()
        if len(parts) >= 3 and parts[1].upper() == 'EQU':
            expr = ' '.join(parts[2:]).replace('$', '0x')
            for k, v in vals.items():
                expr = re.sub(r'\b%s\b' % re.escape(k), str(v), expr)
            try:
                vals[parts[0]] = int(eval(expr, {'__builtins__': {}}))
            except Exception:
                pass
    missing = [n for n in JAG_NAMES if n not in vals]
    if missing:
        raise SystemExit(f"jaguar.inc: cannot evaluate {missing}")
    return {n: vals[n] for n in JAG_NAMES}


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
    for k, v in SIN.items():
        eq.append(('SHIP_' + k, regs['SHIP_IN'][0] + v, "the GPU's ship input (csrc/road.h SIN_" + k + ")"))
    for k, v in CIN.items():
        eq.append(('COMP_' + k, regs['COMP_IN'][0] + v, "GPU composers' input (csrc/road.h CIN_" + k + ")"))
    eq.append(('COMP_ROWS', CIN_ROWS, None))
    for i, o in enumerate(OBJECTS):
        eq.append(('OBJ_' + o, i, 'index in the list'))
    eq += [(k, v, 'switch') for k, v in SWITCHES.items()]
    # Pad bits (Keen Dreams' ReadPad, verified on hardware): PAD_STATE / PAD_KEYS.
    for i, n in enumerate(['UP', 'DOWN', 'LEFT', 'RIGHT', 'A', 'B', 'C', 'OPT', 'PAUSE']):
        eq.append(('PAD_B_' + n, i, 'bit of PAD_STATE'))
    for i in range(10):
        eq.append((f'KEY_B_{i}', i, 'bit of PAD_KEYS'))
    eq += [('KEY_B_STAR', 10, 'bit of PAD_KEYS'), ('KEY_B_HASH', 11, 'bit of PAD_KEYS')]
    eq += [('C_MAGIC', C_MAGIC, "c_init's answer"),
           ('C_ENTRY_INIT', regs['DBLOB'][0], 'C entry table: jmp c_init'),
           ('C_ENTRY_TICK', regs['DBLOB'][0] + 6, 'C entry table: jmp c_tick'),
           ('C_ENTRY_RENDER', regs['DBLOB'][0] + 12, 'C entry table: jmp c_render')]
    eq += [('SCR_W', SCR_W, None), ('SCR_H', SCR_H, None), ('GAME_H', GAME_H, None),
           ('GAME_Y', GAME_Y, None), ('VIEW_H', VIEW_H, None), ('DASH_Y', DASH_Y, None),
           ('DASH_H', DASH_H, None), ('GAME_BYTES', GAME_BYTES, None), ('WORLD_BYTES', WORLD_BYTES, None),
           ('WORLD_STRIDE', align(WORLD_BYTES, 8), None), ('DASH_BYTES', DASH_BYTES, None),
           ('CAR_BYTES', CAR_BYTES, None), ('CAR_FRAMES', CAR_FRAMES, None),
           ('PAL_BYTES', 768, None), ('N_ROADS', 31, None), ('N_WORLDS', 10, None)]

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
        f.write("\n/* From Atari's jaguar.inc. */\n")
        for n, v in jaguar_inc().items():
            f.write(f'#define JAG_{n:<16} 0x{v:X}u\n')
        f.write('\n#endif\n')

    with open(os.path.join(REPO, 'include', 'jag_c.ld'), 'w', newline='\n') as f:
        f.write(f'''/* {hdr} */
OUTPUT_ARCH(m68k)
ENTRY(entry_table)

MEMORY
{{
    BLOB (rwx) : ORIGIN = 0x{regs['DBLOB'][0]:X}, LENGTH = 0x{regs['DBLOB'][1]:X}
    BSS  (rw)  : ORIGIN = 0x{regs['CBSS'][0]:X}, LENGTH = 0x{regs['CBSS'][1]:X}
}}

SECTIONS
{{
    .text : {{ *(.entry) *(.text*) *(.rodata*) . = ALIGN(4); }} > BLOB
    .data : {{ *(.data*) . = ALIGN(4); }} > BLOB
    .bss (NOLOAD) : {{ *(.bss*) *(COMMON) . = ALIGN(4); }} > BSS
    /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
''')

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
