#!/usr/bin/env python3
"""The Jaguar's per-second timing lines (csrc/game_main.c lg_frame) from COM8.

    python tools/comlog.py run ROM.j64 [SECS]   boot the ROM, then listen (default 90 s)
    python tools/comlog.py listen [SECS]        listen only (0: until Ctrl+C)
    python tools/comlog.py show LOG             decode a saved log again

One process does both: jaggd's run, then the serial port (two processes on
the GameDrive at once wedge it: JaguarDocs metodi/gamedrive_usb.md 6.1).
Raw lines go to build/logs/<rom>_<hhmmss>.log; the table goes to stdout.

A line:  S d=DEMO_TICK r=road m=mode f=frames t=ticks x=dropped A=second
         T I G N H D O W L (sums, half-lines) F B P K
         a q p s: Jerry - samples produced, I2S interrupts, idle-loop turns
         with the ring full (all three in the second), starved interrupts
         (since boot)
Columns printed: half-lines PER FRAME, and X = A minus everything measured,
i.e. the time no phase sees (idle STOP when ahead, main loop, interrupts...).
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

PHASES = ['T', 'I', 'G', 'N', 'H', 'D', 'O', 'W', 'L']
NAMES = {'T': 'tick', 'I': 'in', 'G': 'gpu1', 'N': 'ship', 'H': 'gpu2', 'D': 'hud',
         'O': 'ovl', 'W': 'wait', 'L': 'log'}


def parse(line):
    if not line.startswith('S '):
        return None
    return {k: int(v, 16) for k, v in re.findall(r'(\w)=([0-9A-F]+)', line)}


def header():
    cols = ' '.join(f'{NAMES[p]:>5}' for p in PHASES)
    return f'{"demo":>5} {"rd":>2} {"fps":>3} {"tk":>3} {"drp":>3} | {cols} | {"X":>5} {"X%":>4} {"A":>6}'


def row(v):
    f = max(v.get('f', 0), 1)
    meas = sum(v.get(p, 0) for p in PHASES)
    a = v.get('A', 0)
    x = a - meas
    cols = ' '.join(f'{v.get(p, 0) / f:5.0f}' for p in PHASES)
    flags = ''
    if v.get('F') or v.get('B'):
        flags += f'  gpufail={v.get("F")} blitstall={v.get("B")}'
    if v.get('P'):
        flags += f'  logdrop={v.get("P")}'
    if 'a' in v:
        flags += f'  dsp {v["a"]}/s irq {v.get("q", 0)}/s spin {v.get("p", 0)}/s starve {v.get("s", 0)}'
    return (f'{v.get("d", 0):5d} {v.get("r", 0):2d} {v.get("f", 0):3d} {v.get("t", 0):3d} '
            f'{v.get("x", 0):3d} | {cols} | {x / f:5.0f} {100 * x / max(a, 1):4.0f} {a:6d}{flags}')


def listen(secs, log_path):
    import jaggd
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    print(header())
    with open(log_path, 'w', encoding='latin-1') as log:
        for t, ln in jaggd.lines(secs):
            log.write(f'{t:8.3f} {ln}\n')
            log.flush()
            v = parse(ln)
            print(row(v) if v else f'# {ln}', flush=True)
    print(f'log: {log_path}')


def main(argv):
    if not argv:
        sys.exit(__doc__)
    stamp = time.strftime('%H%M%S')
    if argv[0] == 'run':
        import jaggd
        rom = argv[1]
        secs = int(argv[2]) if len(argv) > 2 else 90
        jaggd.run_rom(rom)
        name = os.path.splitext(os.path.basename(rom))[0]
        listen(secs, os.path.join(REPO, 'build', 'logs', f'{name}_{stamp}.log'))
    elif argv[0] == 'listen':
        secs = int(argv[1]) if len(argv) > 1 else 0
        listen(secs, os.path.join(REPO, 'build', 'logs', f'listen_{stamp}.log'))
    elif argv[0] == 'show':
        print(header())
        for ln in open(argv[1], encoding='latin-1'):
            parts = ln.strip().split(' ', 1)
            v = parse(parts[1]) if len(parts) > 1 else None
            if v:
                print(row(v))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
