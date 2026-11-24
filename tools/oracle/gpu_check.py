#!/usr/bin/env python3
"""The GPU's composers (src/gpu_road.s) against road_compose (csrc/road.c).

    python tools/oracle/gpu_check.py [STRIDE]

Builds tools/oracle/gpu_frames.c, which writes, for every road and every
STRIDE-th position (default 1: all of them), what the 68000 hands the GPU and
what road_compose composes; then runs the REAL GPU binary (from build/main.bin,
located through build/main.lst) in tools/oracle/jrisc.py on a DRAM image laid
out as on the Jaguar, frame after frame with the registers carried over, up
to the rasteriser's entry, and compares the two lists and the second
launch's address.  On every 10th frame it goes on: list 1, a ship and
its shadow (SHIP_IN, at poses gpu_frames.c picks), list 2 - all in the same
launch - over a screen of 0xA5, compared pixel for pixel with raster.h,
road_ship and road_shadow_draw.  Run after build.bat (it needs the build's main.bin,
map and cart/).  Exit 1 on the first difference.
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

MINGW = os.environ.get('MINGW', r'C:\msys64\mingw64\bin')   # build.bat's, else this machine's


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
        raise SystemExit(f'gpu_check: {missing} not in build/main.lst')
    return syms


def main():
    stride = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    eq = map_inc()
    sy = lst_symbols(['GpuRom', 'GpuRomEnd', 'GpuRoad', 'gr_start', 'gr_done'])
    main_bin = open(os.path.join(REPO, 'build', 'main.bin'), 'rb').read()
    code = main_bin[sy['GpuRom']:sy['GpuRomEnd']]
    if sy['GpuRoad'] != jrisc.G_RAM or (code[0] >> 2) not in (35, 38):
        raise SystemExit('gpu_check: GpuRom does not start with GpuRoad (moveq / movei)')

    exe = os.path.join(REPO, 'build', 'gpu_frames.exe')
    out = os.path.join(REPO, 'build', 'gpu_frames.bin')
    env = dict(os.environ, PATH=MINGW + os.pathsep + os.environ['PATH'])
    subprocess.run([os.path.join(MINGW, 'gcc.exe'), '-std=c11', '-O2', '-Wall', '-Wextra', '-o', exe,
                    os.path.join(HERE, 'gpu_frames.c'), os.path.join(REPO, 'csrc', 'road.c'),
                    os.path.join(REPO, 'csrc', 'port', 'play.c')], check=True, env=env)
    subprocess.run([exe, os.path.join(REPO, 'cart'), out, str(stride)], check=True, env=env)
    frames = open(out, 'rb').read()

    dram = bytearray(0x200000)
    trekg = open(os.path.join(REPO, 'cart', 'trekg.bin'), 'rb').read()
    dram[eq['TREKG_BUF']:eq['TREKG_BUF'] + len(trekg)] = trekg
    cin_bytes = eq['COMP_WIN'] - eq['COMP_IN'] + eq['COMP_ROWS'] * 32
    cin_at, cmd_at, gpu_cmd = eq['COMP_IN'], eq['CMD_BUF'], eq['GPU_CMD']
    view_at, view_bytes = eq['VIEW0'], eq['VIEW0_SIZE']
    # the ship's frames and the shadow masks, where the 68000 puts them
    cars = open(os.path.join(REPO, 'cart', 'cars.bin'), 'rb').read()
    dram[eq['CARS_BUF']:eq['CARS_BUF'] + len(cars)] = cars
    tables = open(os.path.join(REPO, 'cart', 'tables.bin'), 'rb').read()
    masks = bytearray()
    for m in range(5):
        for i in range(9):
            row = tables[296 + 261 * m + 29 * i:296 + 261 * m + 29 * i + 29]
            masks += sum(0x80000000 >> j for j in range(29) if row[j]).to_bytes(4, 'big')
    dram[eq['SHADOW_MASKS']:eq['SHADOW_MASKS'] + len(masks)] = masks
    ship_at = eq['SHIP_IN']
    sin_bytes = eq['SHIP_SCLIP'] - ship_at + 9 * 4
    gpu = jrisc.Gpu(dram, code)

    t0 = time.time()
    ofs = n_frames = n_views = steps = 0
    while ofs < len(frames):
        road, pos = struct.unpack_from('>II', frames, ofs)
        ofs += 8
        dram[cin_at:cin_at + cin_bytes] = frames[ofs:ofs + cin_bytes]
        ofs += cin_bytes
        second, n = struct.unpack_from('>II', frames, ofs)
        ofs += 8
        want = frames[ofs:ofs + 4 * n]
        ofs += 4 * n
        dram[cmd_at:cmd_at + 0x2400] = b'\xEE' * 0x2400
        dram[gpu_cmd:gpu_cmd + 4] = bytes(4)
        gpu.pc, gpu.steps = jrisc.G_RAM, 0
        try:
            gpu.run_until(sy['gr_start'])
        except jrisc.JriscError as e:
            raise SystemExit(f'gpu_check: road {road} pos {pos}: {e}')
        steps += gpu.steps
        got = bytes(dram[cmd_at:cmd_at + 4 * n])
        l2 = int.from_bytes(dram[gpu_cmd:gpu_cmd + 4], 'big')
        if got != want or l2 != cmd_at + 4 * second or gpu.r[1] != cmd_at:
            for i in range(0, 4 * n, 8):
                if got[i:i + 8] != want[i:i + 8]:
                    g = struct.unpack_from('>II', got, i)
                    w = struct.unpack_from('>II', want, i)
                    print(f'  long {i // 4}: GPU {g[0]:08X} {g[1]:08X}  C {w[0]:08X} {w[1]:08X}')
                    break
            raise SystemExit(f'gpu_check: road {road} pos {pos}: lists differ '
                             f'(list 2 at ${l2:06X}, C ${cmd_at + 4 * second:06X}; r1 ${gpu.r[1]:06X})')
        has_view, = struct.unpack_from('>I', frames, ofs)
        ofs += 4
        if has_view:
            dram[ship_at:ship_at + sin_bytes] = frames[ofs:ofs + sin_bytes]
            ofs += sin_bytes
            want_view = frames[ofs:ofs + view_bytes]
            ofs += view_bytes
            dram[view_at:view_at + view_bytes] = bytes([0xA5]) * view_bytes
            try:
                # ONE launch: list 1, the ship and its shadow, list 2
                gpu.run_until(sy['gr_done'])
            except jrisc.JriscError as e:
                raise SystemExit(f'gpu_check: road {road} pos {pos} (rasteriser): {e}')
            got_view = bytes(dram[view_at:view_at + view_bytes])
            if got_view != want_view:
                i = next(i for i in range(view_bytes) if got_view[i] != want_view[i])
                raise SystemExit(f'gpu_check: road {road} pos {pos}: pixel ({i % 320},{i // 320}) '
                                 f'GPU {got_view[i]:02X}, raster.h {want_view[i]:02X}')
            n_views += 1
        n_frames += 1
    dt = time.time() - t0
    print(f'gpu_check: {n_frames} frames, GPU lists = road_compose; {n_views} screens, '
          f'GPU rasteriser = raster.h; '
          f'{steps / max(n_frames, 1):.0f} instructions per frame on average ({dt:.0f} s)')


if __name__ == '__main__':
    main()
