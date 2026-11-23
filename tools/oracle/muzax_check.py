#!/usr/bin/env python3
"""tools/muzax.py's streams against skyroads-port's driver, tick by tick.

    python tools/oracle/muzax_check.py [SOURCE] [TICKS]

Builds tools/oracle/muzax_check.c (the port's audio.c with the OPL3
emulator replaced by a recorder), runs it on the user's MUZAX.LZS, and
compares, for every song: the reset writes, then TICKS ticks of the song
(default: two full passes of the longest loop, so every goto_loop is
crossed).  Our stream has the original's 255-tick idle wait between the two
(EXE 0x5889 + ds:0xcbc, see tools/muzax.py); the port starts the song at
once, so our tick 255 + k is its tick k.

Also checks what makes the streams independent of the order songs are played
in: a volume event reads the channel's CURRENT instrument, which a previous
song may have left; every retail song programs a channel before setting its
volume, or the check fails.

Exit 1 on the first difference.
"""
import os
import subprocess
import sys
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import muzax  # noqa: E402
import srdata  # noqa: E402

MINGW = r'C:\msys64\mingw64\bin'


def default_source():
    for p in ('skyroads.zip', 'gamedata'):
        if os.path.exists(os.path.join(REPO, p)):
            return os.path.join(REPO, p)
    raise SystemExit('muzax_check: no SkyRoads source')


def main():
    # [SOURCE] [TICKS], either may be missing (build.bat passes its own %1,
    # which is empty when the default source is used)
    args = [a for a in sys.argv[1:] if a]
    ticks = int(args.pop()) if args and args[-1].isdigit() else 45000
    src = srdata.Source(args[0] if args else default_source())
    data = src.read('MUZAX.LZS')
    build = os.path.join(REPO, 'build')
    os.makedirs(build, exist_ok=True)
    lzs_path = os.path.join(build, 'muzax.lzs')
    open(lzs_path, 'wb').write(data)

    exe = os.path.join(build, 'muzax_check.exe')
    env = dict(os.environ, PATH=MINGW + os.pathsep + os.environ['PATH'])
    subprocess.run([os.path.join(MINGW, 'gcc'), '-std=c11', '-O2', '-w', '-o', exe,
                    os.path.join(HERE, 'muzax_check.c'),
                    os.path.join(REPO, 'ref', 'skyroads-port-main', 'src', 'core', 'lzs.c')],
                   check=True, env=env)
    out = subprocess.run([exe, lzs_path, str(ticks)], check=True, env=env,
                         capture_output=True, text=True).stdout.splitlines()

    # The port's dump.
    port_init, port = None, {}
    cur = None
    for line in out:
        f = line.split()
        if f[0] == 'I':
            port_init = [int(x, 16) for x in f[1:]]
        elif f[0] == 'S':
            cur = int(f[1])
            port[cur] = {'R': None, 'T': []}
        elif f[0] == 'R':
            port[cur]['R'] = [int(x, 16) for x in f[1:]]
        elif f[0] == 'T':
            port[cur]['T'].append([int(x, 16) for x in f[2:]])

    ours = muzax.init_writes()
    if ours != port_init:
        raise SystemExit(f'muzax_check: INIT differs\n ours {ours}\n port {port_init}')

    songs = muzax.load_songs(data)
    total = 0
    for n, (song, n_instr) in enumerate(songs):
        # Order independence: program before volume on every channel.
        ev0 = n_instr * 16
        words = struct.unpack_from('<%dH' % ((len(song) - ev0) // 2), song, ev0)
        programmed = set()
        for w in words:
            op, ch = w & 7, (w >> 4) & 15
            if op == 1:
                programmed.add(ch)
            elif op == 4 and ch not in programmed:
                raise SystemExit(f'muzax_check: song {n} sets channel {ch} volume before programming it')
            elif op == 5:
                break

        stream, loop = muzax.song_stream(song, n_instr)
        t = muzax.ticks_of(stream, loop, ticks + muzax.IDLE_DELAY)
        if t[0] != port[n]['R']:
            raise SystemExit(f'muzax_check: song {n} reset differs')
        if any(t[1:muzax.IDLE_DELAY]):
            raise SystemExit(f'muzax_check: song {n} writes during the idle wait')
        for k in range(ticks):
            a, b = t[muzax.IDLE_DELAY + k], port[n]['T'][k]
            if a != b:
                raise SystemExit(f'muzax_check: song {n} tick {k} differs\n ours {a}\n port {b}')
        total += sum(len(x) for x in port[n]['T'])
        print(f'  song {n:2}: stream {len(stream):6,} bytes, loop at {loop:6,}, {ticks} ticks equal')
    print(f'muzax_check: init and 14 songs equal to skyroads-port ({total:,} writes)')


if __name__ == '__main__':
    main()
