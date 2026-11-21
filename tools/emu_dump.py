#!/usr/bin/env python3
"""Run the ROM on BigPEmu, dump DRAM and the 68000's registers at given frames,
close the emulator, and print what the boot reached.

    python tools/emu_dump.py [--frames 700,900] [--rom build/skyroads.j64]

Why not a screenshot: PrintWindow on BigPEmu's window is unreliable (Prowl
caught the BIOS logo with it), while the DRAM says exactly where the program
is.  The method and the script are Prowl's (translate/pack.py).

The first line of the script is required by BigPEmuDev; fs_open writes under
its Scripts directory; the emulated BIOS takes more than 600 frames.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import memory_map as mm

REPO = mm.REPO
BIGPEMU = os.environ.get('SR_BIGPEMU', r'D:\Homebrew6\Jaguar-KeenDreams\BigPEmuDEV')
KEY = 'jagsr'

SCRIPT = r'''//__BIGPEMU_SCRIPT_MODULE__
//__BIGPEMU_META_DESC__		"JagSkyroads: dump DRAM at given frames, then close"
//__BIGPEMU_META_AUTHOR__	"JagSkyroads tools/emu_dump.py"
#include "bigpcrt.h"

static const uint64_t skFrames[] = { %(frames)s };
static const uint32_t skCount = %(count)d;
static uint32_t sNext = 0;
static uint8_t sBuf[0x10000];

static void put32(const uint64_t h, const uint32_t v)
{
	const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	fs_write(b, 4, h);
}

static void dump(const uint64_t f)
{
	char name[96];
	sprintf(name, "%(key)s_f%%d.dram", (int)f);
	const uint64_t fh = fs_open(name, 1);
	if (!fh) return;
	for (uint32_t a = 0; a < 0x200000; a += 0x10000)
	{
		bigpemu_jag_sysmemread(sBuf, a, 0x10000);
		fs_write(sBuf, 0x10000, fh);
	}
	fs_close(fh);
	sprintf(name, "%(key)s_f%%d.m68k", (int)f);
	const uint64_t ch = fs_open(name, 1);
	if (ch)
	{
		put32(ch, bigpemu_jag_m68k_get_pc());
		for (uint32_t i = 0; i < 8; i++) put32(ch, bigpemu_jag_m68k_get_dreg(i));
		for (uint32_t i = 0; i < 8; i++) put32(ch, bigpemu_jag_m68k_get_areg(i));
		fs_close(ch);
	}
}

static uint32_t on_emu_frame(const int32_t eventHandle, void *pEventData)
{
	const uint64_t f = bigpemu_jag_get_frame_count();
	if (sNext < skCount && f >= skFrames[sNext])
	{
		dump(skFrames[sNext]);
		sNext++;
		if (sNext == skCount)
		{
			const uint64_t dh = fs_open("%(key)s_done.txt", 1);
			if (dh) { fs_write("done", 4, dh); fs_close(dh); }
			EBigPEmuWindowPlatform plat;
			const uint64_t w = bigpemu_native_window_handle(&plat);
			if (w) bigpemu_send_native_window_message(w, 0x0010, 0, 0);
		}
	}
	return 0;
}

void bigp_init()
{
	void *pMod = bigpemu_get_module_handle();
	bigpemu_register_event_emu_thread_frame(pMod, on_emu_frame);
}

void bigp_shutdown()
{
}
'''


def kill_emu():
    subprocess.run(['taskkill', '/IM', 'BigPEmuDev.exe', '/F'], capture_output=True)
    time.sleep(0.6)


def run(rom, frames, timeout=90):
    exe = os.path.join(BIGPEMU, 'BigPEmuDev.exe')
    scripts = os.path.join(BIGPEMU, 'Scripts')
    src = os.path.join(scripts, f'{KEY}_dump.c')
    vm = os.path.join(scripts, f'{KEY}_dump.bigpcvm')
    # Other projects' scripts in the same folder would run too: park them.
    parked = []
    for n in os.listdir(scripts):
        if n.endswith('.c') and not n.startswith(KEY):
            os.rename(os.path.join(scripts, n), os.path.join(scripts, n + '.off'))
            parked.append(n)
    try:
        with open(src, 'w', newline='\n') as f:
            f.write(SCRIPT % dict(frames=', '.join(map(str, frames)), count=len(frames), key=KEY))
        kill_emu()
        if os.path.exists(vm):
            os.remove(vm)
        p = subprocess.Popen([exe])                  # compiles the scripts at start-up
        t0 = time.time()
        while not os.path.exists(vm):
            if time.time() - t0 > 30:
                p.kill()
                raise SystemExit('BigPEmuDev did not compile the script (see its log)')
            time.sleep(0.3)
        time.sleep(0.5)
        p.kill()
        kill_emu()
        done = os.path.join(scripts, f'{KEY}_done.txt')
        for n in os.listdir(scripts):
            if n.startswith(KEY + '_f') or n == os.path.basename(done):
                os.remove(os.path.join(scripts, n))
        p = subprocess.Popen([exe, os.path.abspath(rom)])
        t0 = time.time()
        while not os.path.exists(done):
            if time.time() - t0 > timeout:
                print(f'emu_dump: not all frames reached within {timeout} s')
                break
            time.sleep(0.5)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
        kill_emu()
        out = []
        for fr in frames:
            res = {}
            for ext in ('dram', 'm68k'):
                s = os.path.join(scripts, f'{KEY}_f{fr}.{ext}')
                d = os.path.join(REPO, 'build', f'emu_f{fr}.{ext}')
                if os.path.exists(s):
                    shutil.move(s, d)
                    res[ext] = d
            if len(res) == 2:
                out.append((fr, res))
        if not out:
            print('emu_dump: no frame reached at all')
        return out
    finally:
        for n in parked:
            os.rename(os.path.join(scripts, n + '.off'), os.path.join(scripts, n))


def syms():
    """The EQUs of include/map.inc."""
    d = {}
    for line in open(os.path.join(REPO, 'include', 'map.inc')):
        parts = line.split()
        if len(parts) >= 3 and parts[1] == 'EQU':
            d[parts[0]] = int(parts[2].lstrip('$'), 16)
    return d


def report(fr, res):
    ram = open(res['dram'], 'rb').read()
    regs = struct.unpack('>17I', open(res['m68k'], 'rb').read())
    s = syms()
    u32 = lambda a: struct.unpack_from('>I', ram, a)[0]
    u16 = lambda a: struct.unpack_from('>H', ram, a)[0]
    print(f"frame {fr}: PC ${regs[0]:06X}  SP ${regs[16]:06X}")
    print(f"  BOOT_MARK {u32(s['BOOT_MARK'])}  IRQ_TICK {u32(s['IRQ_TICK'])}  "
          f"NTSC {u16(s['VID_NTSC'])}  VDB {u16(s['VDB_VAL'])} VDE {u16(s['VDE_VAL'])} VI {u16(s['VI_VAL'])}")
    op = s['OP_LIST']
    for i in range(0, s['OP_LIST_SIZE'], 8):
        print(f"  op+{i:02X}: {ram[op + i:op + i + 8].hex(' ')}")
    return ram


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.path.join(REPO, 'build', 'skyroads.j64'))
    ap.add_argument('--frames', default='700,900')
    ap.add_argument('--timeout', type=int, default=90)
    ap.add_argument('--last', action='store_true', help='report only the last frame reached')
    a = ap.parse_args()
    got = run(a.rom, [int(x) for x in a.frames.split(',')], a.timeout)
    print('frames reached:', [fr for fr, _ in got])
    for fr, res in (got[-1:] if a.last else got):
        report(fr, res)


if __name__ == '__main__':
    main()
