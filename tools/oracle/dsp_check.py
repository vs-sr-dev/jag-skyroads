#!/usr/bin/env python3
"""Jerry's OPL2 (src/dsp_audio.s) against its specification (opl2_model.c).

    python tools/oracle/dsp_check.py [SECONDS] [SONG] [SFX] [now]

Runs the REAL DSP binary (from build/main.bin, located through
build/main.lst) in tools/oracle/jrisc.py, on a DRAM image laid out as on the
Jaguar and the cartridge's music and effects, and plays the I2S itself: at
dsp_emitted it takes the sample the loop has just produced and empties the
ring, so the loop never waits.  The scenario is InitDspAudio's (INIT posted
before the start), then song SONG (default 3) posted after the first tick and
effect SFX (default 1) a fifth of the way in, both through the 68000's cells.
With `now` the song starts past its stream's head (the reset and the
original's 1.4 s wait): music from the first tick, for the build.
tools/oracle/opl2_dump.c plays the same scenario through the model; every
sample must be equal.

Also reports what a sample costs, counted as JaguarDocs hw/06 section 12
measures the GPU (1 clk an ALU instruction, 3 a movei, 4 more for a taken
jump, 2 a load or store in local RAM, 4.3 an indexed one, ~40 an external
load): an estimate, not a measurement - the hardware's is the SND_SPIN
counter in the COM8 log.  Exit 1 on the first difference.
"""
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import jrisc  # noqa: E402

MINGW = r'C:\msys64\mingw64\bin'


def map_inc():
    eq = {}
    for line in open(os.path.join(REPO, 'include', 'map.inc')):
        m = re.match(r'(\w+)\s+EQU\s+\$([0-9A-Fa-f]+)', line)
        if m:
            eq[m.group(1)] = int(m.group(2), 16)
    return eq


def lst_symbols(names):
    syms = {}
    for line in open(os.path.join(REPO, 'build', 'main.lst'), errors='replace'):
        for m in re.finditer(r'(\S+) ([0-9A-F]{16})  [a-z]', line):
            if m.group(1) in names:
                syms[m.group(1)] = int(m.group(2), 16)
    missing = [n for n in names if n not in syms]
    if missing:
        raise SystemExit(f'dsp_check: {missing} not in build/main.lst')
    return syms


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
    song = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    sfx = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    now = len(sys.argv) > 4 and sys.argv[4] == 'now'
    eq = map_inc()
    sy = lst_symbols(['DspAudioRom', 'DspAudioRomEnd', 'dsp_emitted', 'snd_rd',
                      'dsp_ram_end', 'opl_dinc', 'dsp_spin', 'dsp_idle', 'prof_eg',
                      'prof_ch', 'prof_rhy', 'prof_out', 'seq_tick'])
    regions = sorted([(sy['dsp_idle'], 'tick+lfo'), (sy['prof_eg'], 'envelopes'),
                      (sy['prof_ch'], 'channels'), (sy['prof_rhy'], 'rhythm'),
                      (sy['prof_out'], 'out'), (sy['seq_tick'], 'slow paths')])
    prof = {}
    if sy['dsp_ram_end'] > jrisc.D_RAM + jrisc.D_RAM_SIZE:
        raise SystemExit(f"dsp_check: the DSP blob ends at ${sy['dsp_ram_end']:06X}, past Jerry's 8 KB")
    main_bin = open(os.path.join(REPO, 'build', 'main.bin'), 'rb').read()
    code = main_bin[sy['DspAudioRom']:sy['DspAudioRomEnd']]
    mz = open(os.path.join(REPO, 'cart', 'muzax.bin'), 'rb').read()
    sx = open(os.path.join(REPO, 'cart', 'sfx.bin'), 'rb').read()

    fs = None
    for line in open(os.path.join(REPO, 'build', 'opl2_tab.h')):
        if line.startswith('#define OPL_FS '):
            fs = float(line.split()[2])
    n = int(secs * fs)
    at_song, at_sfx = 200, n // 5
    skip = 0
    if now:                         # past the reset and the wait: music at once
        st = be32(mz, 4 + 8 * (1 + song))
        while True:
            e = struct.unpack_from('>H', mz, st + skip)[0]
            skip += 2
            if 0 < e < 0x100:
                break

    # The model.
    exe = os.path.join(REPO, 'build', 'opl2_dump.exe')
    out = os.path.join(REPO, 'build', 'opl2_dump.bin')
    env = dict(os.environ, PATH=MINGW + os.pathsep + os.environ['PATH'])
    subprocess.run([os.path.join(MINGW, 'gcc.exe'), '-std=c11', '-O2', '-Wall', '-Wextra', '-o', exe,
                    os.path.join(HERE, 'opl2_dump.c'), os.path.join(HERE, 'opl2_model.c')],
                   check=True, env=env)
    subprocess.run([exe, os.path.join(REPO, 'cart', 'muzax.bin'), os.path.join(REPO, 'cart', 'sfx.bin'),
                    str(n), str(1 + song), str(at_song), str(sfx), str(at_sfx), out, str(skip)],
                   check=True, env=env)
    want = struct.unpack('<%dh' % n, open(out, 'rb').read())

    # The Jaguar: DRAM with the slow tables and the cells, the cartridge.
    dram = bytearray(0x200000)
    tabs = main_bin[sy['opl_dinc']:sy['opl_dinc'] + 640]
    dram[eq['OPL_TABS']:eq['OPL_TABS'] + 640] = tabs

    def cell(name, v):
        dram[eq[name]:eq[name] + 4] = struct.pack('>I', v & 0xFFFFFFFF)

    def post_song(k, skip=0):
        cell('MUS_ADR', eq['ROM_MUZAX'] + be32(mz, 4 + 8 * k) + skip)
        cell('MUS_LOOP', eq['ROM_MUZAX'] + be32(mz, 8 + 8 * k))
        cell('MUS_SEQ', be32(dram, eq['MUS_SEQ']) + 1)

    post_song(0)                                            # INIT, as InitDspAudio
    dsp = jrisc.Dsp(dram, code, rom=[(eq['ROM_MUZAX'], mz), (eq['ROM_SFX'], sx)])
    emitted, rd_at = sy['dsp_emitted'], sy['snd_rd'] - jrisc.D_RAM

    t0 = time.time()
    costs = []
    for i in range(n):
        if i == at_song:
            post_song(1 + song, skip)
        if i == at_sfx and sfx >= 0:
            cell('SND_ADR', eq['ROM_SFX'] + be32(sx, 4 + 8 * sfx))
            cell('SND_LEN', be32(sx, 8 + 8 * sfx))
            cell('SND_SEQ', be32(dram, eq['SND_SEQ']) + 1)
        clk = prev_clk = 0.0
        limit = dsp.steps + 200000
        while True:
            pc = dsp.pc
            if pc == emitted and i == len(costs) - 0 and False:
                pass
            w = dsp.word(pc)
            op = w >> 10
            if op == 38:
                clk += 3
            elif op in (43, 44, 49, 50, 58, 59, 60, 61):
                clk += 4.3
            elif op in (41, 47):
                a = dsp.r[(w >> 5) & 31]
                clk += 40 if (op == 41 and jrisc.Dsp.external(a)) else 2
            elif op in (52, 53):
                clk += 1 + (4 if dsp.cond(w & 31) else 0)
            else:
                clk += 1
            reg = 'init'
            for a, name in regions:
                if pc >= a:
                    reg = name
            prof[reg] = prof.get(reg, 0) + (clk - prev_clk)
            prev_clk = clk
            try:
                dsp.step()
            except jrisc.JriscError as e:
                raise SystemExit(f'dsp_check: sample {i}: {e}')
            if dsp.pc == emitted:
                break
            if dsp.pc == sy['dsp_spin']:
                raise SystemExit(f'dsp_check: sample {i}: the loop thinks the ring is full')
            if dsp.steps > limit:
                raise SystemExit(f'dsp_check: sample {i}: no sample after 200,000 instructions '
                                 f'(pc ${dsp.pc:06X})')
        got = dsp.r[24] - (1 << 32) if dsp.r[24] >> 31 else dsp.r[24]
        if got != want[i]:
            raise SystemExit(f'dsp_check: sample {i} (t = {i / fs:.4f} s): DSP {got}, model {want[i]}')
        costs.append(clk)
        # the I2S, instantly: the ring is empty again
        dsp.local[rd_at:rd_at + 4] = struct.pack('>I', dsp.r[23])
    dt = time.time() - t0
    srt = sorted(costs)
    budget = 26590906 / fs - 54
    print(f'dsp_check: {n} samples equal to the model ({secs:.2f} s of song {song}, effect {sfx}; '
          f'{dt:.0f} s); per sample ~{sum(costs) / n:.0f} clk on average, 99% under '
          f'{srt[int(0.99 * n)]:.0f}, worst {srt[-1]:.0f} (budget {budget:.0f})')
    print('  per sample: ' + ', '.join(f'{k} {v / n:.0f}' for k, v in sorted(prof.items(), key=lambda x: -x[1])))
    # What starves the I2S is not one dear sample (the 64-sample ring takes
    # it) but a run of them: the worst average over 64 consecutive samples.
    # From 1.6 s: past the song's 1.42 s wait and the reset burst before it
    # (silence: a starved ISR repeats a zero).
    m0 = min(int((0.05 if now else 1.6) * fs), n - 64)
    acc = sum(costs[m0:m0 + 64])
    worst64 = acc
    for i in range(m0 + 64, n):
        acc += costs[i] - costs[i - 64]
        worst64 = max(worst64, acc)
    print(f'  music only: average {sum(costs[m0:]) / max(1, n - m0):.0f} clk, '
          f'worst 64-sample average {worst64 / 64:.0f} clk: fits SCLK >= '
          f'{max(0, int((worst64 / 64 + 54) / 64 + 0.999) - 1)}')


if __name__ == '__main__':
    main()
