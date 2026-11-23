#!/usr/bin/env python3
"""The OPL2 on Jerry: its constants and tables, ONE truth for the DSP and the PC.

    python tools/opl2.py

Writes (generated, never edited by hand, git-ignored):
  include/opl2.inc      EQUs and LOGEXP, assembled into the DSP blob (local RAM)
  include/opl2_rom.inc  DINC, AINC, MULT, KSL: read by the DSP's slow paths only,
                        so they live with the 68000's code (DRAM), not in
                        Jerry's 8 KB
  build/opl2_tab.h      the same numbers for the C model (tools/oracle/opl2_model.c)

The synthesiser runs at the I2S rate the SCLK in tools/memory_map.py gives,
Fs = clk / (64 (SCLK + 1)) (JaguarDocs hw/07 rule 1, exact on hardware), not
at the YM3812's 49,716 Hz.  So nothing here is a copy of a chip's timing:
every rate is the CHIP's rate rescaled to Fs, with R = 49716 / Fs chip
samples per Jaguar sample.

Tables (all longs: Jerry's local RAM is read 32 bits at a time, JaguarDocs
hw/06 rule 4; a word table read with loadw returns the whole long).  In the
DSP's RAM LOGSIN and EXP are ONE table, LOGEXP = LOGSIN | EXP << 16: 1 KB
saved for an `and` and a `shrq` per operator.
  LOGSIN  256  -log2(sin) of the quarter wave, 4.8 fixed (the chip's ROM,
               from its closed form; equal to Nuked OPL3's table, checked by
               tools/oracle/opl2_check.py)
  EXP     256  2^x mantissas, ALREADY doubled as the chip doubles them
  DINC     64  linear envelope step, 16.16, by rate
  AINC     64  attack coefficient (16-bit fraction), by rate
               (both per ENVELOPE STEP: a slot's envelope advances every
               EG_EVERY-th sample, a quarter of the slots each sample, as
               JagLBA did every 8th: the envelopes are the cost, not the ear)
  MULT     16  the multiplier's phase factor: inc = (fnum << block) * MULT
  KSL      16  the key-scale ROM, times 4
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import memory_map  # noqa: E402

CLK = 26590906                     # NTSC system clock (PAL differs by 0.01 %)
SCLK = memory_map.AUDIO_SCLK
FS = CLK / (64 * (SCLK + 1))
OPL_RATE = 3579545 / 72            # the YM3812's sample rate
R = OPL_RATE / FS                  # chip samples per Jaguar sample
EG_EVERY = 4                       # each envelope advances every 4th sample
TICK_HZ = 1193182 / 6628           # MUZAX's timer, 180.02 Hz (EXE 0x3ADE)

MT = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30]   # 2 x multiple
KSLROM = [0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64]


def logsin():
    return [round(-math.log2(math.sin((i + 0.5) * math.pi / 512)) * 256) for i in range(256)]


def exp_doubled():
    # The chip's exp ROM (descending, implicit 1024 included), times two:
    # out = EXP[level & 255] >> (level >> 8).
    return [(round((2 ** ((255 - i) / 256) - 1) * 1024) + 1024) << 1 for i in range(256)]


def decay_per_chip_sample(rate):
    """Average envelope increment per chip sample, from the YM3812's EG timer.

    Derived from the counter rule (Nuked OPL3's OPL3_EnvelopeCalc): below
    rate_hi 12 a step of 1 when rate_hi + ctz(timer) + 1 hits 12 (and 13, 14
    by rate_lo), the timer advancing every other sample; from 12 up the
    step pattern table.  Both give (4 + rl) 2^(rh - 15); rate_hi 15 is 4.
    """
    rh, rl = rate >> 2, rate & 3
    if rh == 0:
        return 0.0
    if rh >= 15:
        return 4.0
    return (4 + rl) * 2.0 ** (rh - 15)


INCSTEP = [[0, 0, 0, 0], [1, 0, 0, 0], [1, 0, 1, 0], [1, 1, 1, 0]]


def attack_chip_samples(rate):
    """Chip samples to attack from 511 to 0 (Nuked's rule, simulated)."""
    rh, rl = rate >> 2, rate & 3
    if rh == 0:
        return math.inf
    if rh >= 15:
        return 0.0

    def step(r, shift):
        return r + ((~r) >> (4 - shift))          # Python >> is arithmetic

    if rh < 12:
        # Events of shift 1 at the decay rate; count them.
        r, n = 511, 0
        while r:
            r, n = step(r, 1), n + 1
        return n / decay_per_chip_sample(rate)
    r, t, timer, eg_state = 511, 0, 0, 0
    lo = 0
    while r:
        if eg_state:
            lo = timer & 3
        shift = (rh & 3) + INCSTEP[rl][lo]
        if shift & 4:
            shift = 3
        if not shift:
            shift = eg_state
        if shift:
            r = step(r, shift)
        if eg_state:
            timer += 1
        eg_state ^= 1
        t += 1
    return float(t)


def tables():
    dinc = [round(decay_per_chip_sample(r) * R * EG_EVERY * 65536) for r in range(64)]
    ainc = []
    for r in range(64):
        t = attack_chip_samples(r)
        if t == math.inf:
            ainc.append(0)
            continue
        n = t / R / EG_EVERY                      # envelope steps, 512 -> 1
        a = 1.0 if n < 1 else 1 - math.exp(-math.log(512) / n)
        ainc.append(max(1, min(65535, round(a * 65536))))
    # Phase: 32 bits, the wave index in the top 10 (the chip's 19-bit phase
    # << 13).  Chip increment = ((fnum << block) >> 1) * MT >> 1 per chip
    # sample, so per Jaguar sample (fnum << block) * MT * R * 2^11.
    mult = [round(m * R * 2048) for m in MT]
    return {
        'LOGSIN': logsin(),
        'EXP': exp_doubled(),
        'DINC': dinc,
        'AINC': ainc,
        'MULT': mult,
        'KSL': [k * 4 for k in KSLROM],
    }


def constants():
    return {
        'OPL_SCLK': SCLK,
        # 32-bit accumulators whose carry is the event: the MUZAX tick, and
        # one LFO step every 64 chip samples (tremolo; vibrato every 16th).
        'OPL_TICK_INC': round(TICK_HZ / FS * 2 ** 32),
        'OPL_LFO_INC': round(R / 64 * 2 ** 32),
        'OPL_FS_X100': round(FS * 100),
    }


def main():
    t = tables()
    c = constants()
    os.makedirs(os.path.join(REPO, 'include'), exist_ok=True)
    os.makedirs(os.path.join(REPO, 'build'), exist_ok=True)
    hdr = 'GENERATED BY tools/opl2.py - DO NOT EDIT BY HAND.'
    def table(f, name, vals):
        f.write(f'    .long\nopl_{name}:\n')
        for i in range(0, len(vals), 8):
            f.write('    dc.l    ' + ', '.join(f'${v & 0xFFFFFFFF:X}' for v in vals[i:i + 8]) + '\n')

    with open(os.path.join(REPO, 'include', 'opl2.inc'), 'w', newline='\n') as f:
        f.write(f'; {hdr}\n; Fs = {FS:.2f} Hz (SCLK {SCLK}), R = {R:.6f}\n\n')
        for k, v in c.items():
            f.write(f'{k:<16} EQU     ${v:X}\n')
        f.write('\n')
        table(f, 'logexp', [a | b << 16 for a, b in zip(t['LOGSIN'], t['EXP'])])
    with open(os.path.join(REPO, 'include', 'opl2_rom.inc'), 'w', newline='\n') as f:
        f.write(f'; {hdr}\n; Fs = {FS:.2f} Hz (SCLK {SCLK})\n\n')
        for k in ('DINC', 'AINC', 'MULT', 'KSL'):
            table(f, k.lower(), t[k])
    with open(os.path.join(REPO, 'build', 'opl2_tab.h'), 'w', newline='\n') as f:
        f.write(f'/* {hdr} */\n/* Fs = {FS:.2f} Hz (SCLK {SCLK}), R = {R:.6f} */\n#include <stdint.h>\n\n')
        f.write(f'#define OPL_FS {FS!r}\n#define OPL_R {R!r}\n')
        for k, v in c.items():
            f.write(f'#define {k} 0x{v:X}u\n')
        for k, vals in t.items():
            f.write(f'static const uint32_t OPL_{k}[{len(vals)}] = {{\n')
            for i in range(0, len(vals), 8):
                f.write('    ' + ', '.join(f'0x{v & 0xFFFFFFFF:X}u' for v in vals[i:i + 8]) + ',\n')
            f.write('};\n')
    print(f'opl2: Fs {FS:.2f} Hz (SCLK {SCLK}), R {R:.4f}, {sum(map(len, t.values()))} table longs')


if __name__ == '__main__':
    main()
