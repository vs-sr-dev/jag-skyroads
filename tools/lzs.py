"""Bluemoon LZS bitstream and decompressor.

The algorithm was recovered from SKYROADS.EXE (routine at 0x6660) by the
skyroads-port project (github.com/ammaarreshi/skyroads-port, MIT License,
Copyright (c) 2026 Ammaar Reshi); see its docs/FORMATS.md section 1.  This is a
re-implementation of that specification.

Stream model: a one-byte lookahead register shared by byte and bit reads.
  read_byte()  returns the lookahead and loads the next file byte
  read_bit()   consumes the lookahead MSB-first, reloading after eight bits
  flush()      drops the rest of a partially consumed byte

Block:  u8 len_bits, u8 off_bits, u8 far_bits, then until `size` bytes:
  0            near match, dist = bits(off_bits)
  1 0          far match,  dist = bits(far_bits) + (1 << off_bits)
  1 1          literal,    byte = bits(8)
  match: n = bits(len_bits); n+1 >= remaining terminates; else copy n+2 bytes
         from out_pos - 2 - dist (overlap allowed).
"""


class Stream:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos + 1
        self.cur = data[pos] if pos < len(data) else 0
        self.bits = 8

    def _load(self):
        self.cur = self.data[self.pos] if self.pos < len(self.data) else 0
        self.pos += 1

    def read_byte(self):
        r = self.cur
        self._load()
        return r

    def read_u16(self):
        lo = self.read_byte()
        return lo | (self.read_byte() << 8)

    def read_bytes(self, n):
        return bytes(self.read_byte() for _ in range(n))

    def read_bit(self):
        bit = (self.cur >> 7) & 1
        self.cur = (self.cur << 1) & 0xFF
        self.bits -= 1
        if self.bits == 0:
            self.bits = 8
            self._load()
        return bit

    def read_bits(self, n):
        v = 0
        for i in range(n - 1, -1, -1):
            v |= self.read_bit() << i
        return v

    def flush(self):
        if self.bits != 8:
            self.read_bits(self.bits)

    @property
    def offset(self):
        """File offset of the lookahead byte."""
        return self.pos - 1


def decompress(s, size):
    out = bytearray()
    len_bits = s.read_byte()
    off_bits = s.read_byte()
    far_bits = s.read_byte()
    far_bias = 1 << off_bits
    while len(out) < size:
        if s.read_bit() == 0:
            dist = s.read_bits(off_bits)
        elif s.read_bit() == 0:
            dist = s.read_bits(far_bits) + far_bias
        else:
            out.append(s.read_bits(8))
            continue
        n = s.read_bits(len_bits)
        if n + 1 >= size - len(out):
            break
        src = len(out) - 2 - dist
        for i in range(n + 2):
            out.append(out[src + i])
    s.flush()
    if len(out) < size:
        out.extend(bytes(size - len(out)))
    return bytes(out)
