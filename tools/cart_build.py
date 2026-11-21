#!/usr/bin/env python3
"""Cartridge packer: every blob at the offset tools/memory_map.py decided.

    python tools/cart_build.py [--out FILE]

Concatenating with .incbin would make every data address depend on the size of
the assembled code; here they are fixed by contract, and a blob that outgrows
its region is a build error instead of a silent overwrite.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import memory_map as mm
from cart_header import write_boot_header

REPO = mm.REPO


def main():
    out = os.path.join(REPO, 'build', 'skyroads.j64')
    if '--out' in sys.argv:
        out = sys.argv[sys.argv.index('--out') + 1]

    lay, _ = mm.cart_layout()
    cart = bytearray(b'\xFF' * mm.cart_size())
    code_addr = lay[0][1]
    write_boot_header(cart, code_addr)

    for sym, addr, ofs, size, fn in lay:
        if sym == 'ROM_CODE':
            rel = os.path.join('build', 'main.bin')
        elif sym == 'ROM_CBLOB':
            rel = os.path.join('build', 'engine.bin')
        else:
            rel = os.path.join('cart', fn)
        data = open(os.path.join(REPO, rel), 'rb').read()
        if len(data) > size:
            raise SystemExit(f"{rel}: {len(data):,} bytes do not fit {sym} ({size:,})")
        cart[ofs:ofs + len(data)] = data
        print(f"  {sym:<11} ${addr:06X} {len(data):>9,} bytes <- {rel}")

    open(out, 'wb').write(bytes(cart))
    print(f"cart_build: {os.path.relpath(out, REPO)} ({len(cart) >> 20} MB, entry ${code_addr:06X})")


if __name__ == '__main__':
    main()
