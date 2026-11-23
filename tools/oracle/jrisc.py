"""A small JRISC (TOM's GPU) interpreter: the logic of src/gpu_road.s on the PC.

Adapted from Prowl's translate/jrisc.py (the user's own project).  Only what
gpu_road.s uses; the delay slot of JR/JUMP and the conditions as the TRM
gives them (C = borrow on subtraction).  No timing, no scoreboard, no write
posting: it tells LOGIC from hardware, and the hardware half is answered on
the Jaguar (keypad 9, csrc/game_main.c).

A long access ignores the two low address bits, as the GPU does.
"""
M32 = 0xFFFFFFFF
G_RAM = 0xF03000
B_CMD = 0xF02238
G_HIDATA = 0xF02118


class JriscError(RuntimeError):
    pass


class Gpu:
    def __init__(self, dram, code, org=G_RAM):
        self.dram = dram                                # bytearray, 2 MB, shared with the caller
        self.local = bytearray(0x1000)
        self.local[:len(code)] = code
        self.r = [0] * 32
        self.pc = org
        self.z = self.c = self.n = False
        self.steps = 0
        self.hidata = 0xBAD0BAD0

    def poison(self, a):
        # Every external load rewrites G_HIDATA with whatever crossed the bus
        # (JaguarDocs hw/06, T-GPU-16, hardware).  Here it becomes garbage, so
        # a storep that relies on a G_HIDATA set before a load fails on the
        # PC as well.
        self.hidata = (0xBAD00000 ^ a ^ self.steps) & M32

    def rd32(self, a):
        a &= 0xFFFFFC
        if a < 0x200000:
            self.poison(a)
            return int.from_bytes(self.dram[a:a + 4], 'big')
        if G_RAM <= a < G_RAM + 0x1000:
            o = a - G_RAM
            return int.from_bytes(self.local[o:o + 4], 'big')
        if a == B_CMD:
            self.poison(a)
            return 1                                    # the Blitter idle
        raise JriscError(f'load from ${a:06X} at ${self.pc - 2:06X}')

    def wr(self, a, v, n):
        a &= 0xFFFFFF
        if a < 0x200000:
            self.dram[a:a + n] = (v & ((1 << 8 * n) - 1)).to_bytes(n, 'big')
        elif G_RAM <= a < G_RAM + 0x1000:
            o = a - G_RAM
            self.local[o:o + n] = (v & ((1 << 8 * n) - 1)).to_bytes(n, 'big')
        elif a == G_HIDATA and n == 4:
            self.hidata = v & M32
        else:
            raise JriscError(f'store to ${a:06X} at ${self.pc - 2:06X}')

    def word(self, a):
        o = a - G_RAM
        if not 0 <= o < 0x1000:
            raise JriscError(f'execution outside the GPU RAM: ${a:06X}')
        return int.from_bytes(self.local[o:o + 2], 'big')

    def cond(self, cc):
        flag = self.n if cc & 16 else self.c
        if cc & 1 and self.z:
            return False
        if cc & 2 and not self.z:
            return False
        if cc & 4 and flag:
            return False
        if cc & 8 and not flag:
            return False
        return True

    def zn(self, v):
        v &= M32
        self.z = v == 0
        self.n = bool(v >> 31)
        return v

    def step(self, in_slot=False):
        """One instruction (and, for a taken jump, its delay slot)."""
        w = self.word(self.pc)
        op, s, d = w >> 10, (w >> 5) & 31, w & 31
        self.pc += 2
        r = self.r
        self.steps += 1
        if op == 0:                                     # add
            t = r[d] + r[s]
            self.c = t > M32
            r[d] = self.zn(t)
        elif op == 2:                                   # addq
            t = r[d] + (s or 32)
            self.c = t > M32
            r[d] = self.zn(t)
        elif op == 4:                                   # sub
            self.c = r[s] > r[d]
            r[d] = self.zn(r[d] - r[s])
        elif op == 6:                                   # subq
            n = s or 32
            self.c = n > r[d]
            r[d] = self.zn(r[d] - n)
        elif op == 8:                                   # neg
            self.c = r[d] != 0
            r[d] = self.zn(-r[d])
        elif op == 9:
            r[d] = self.zn(r[d] & r[s])
        elif op == 10:
            r[d] = self.zn(r[d] | r[s])
        elif op == 11:
            r[d] = self.zn(r[d] ^ r[s])
        elif op == 13:                                  # btst: Z = the bit is clear
            self.z = not (r[d] >> s) & 1
        elif op == 24:                                  # shlq: encoded 32 - n
            n = 32 - s
            self.c = bool(r[d] >> 31)
            r[d] = self.zn(r[d] << n)
        elif op == 25:                                  # shrq
            n = s or 32
            self.c = bool(r[d] & 1)
            r[d] = self.zn(r[d] >> n)
        elif op == 30:                                  # cmp
            self.c = r[s] > r[d]
            self.zn(r[d] - r[s])
        elif op == 31:                                  # cmpq: signed 5 bits
            n = s - 32 if s & 16 else s
            self.c = (n & M32) > r[d]
            self.zn(r[d] - n)
        elif op == 34:                                  # move
            r[d] = r[s]
        elif op == 35:                                  # moveq
            r[d] = s
        elif op == 38:                                  # movei: low word first
            lo, hi = self.word(self.pc), self.word(self.pc + 2)
            self.pc += 4
            r[d] = hi << 16 | lo
            # On the real TOM movei was seen to change the flags (against the
            # TRM).  Here it always does, so code that tests flags across a
            # movei goes wrong on the PC too.
            self.zn(r[d] ^ 0x80000001)
            self.c = not self.c
        elif op == 41:                                  # load
            r[d] = self.rd32(r[s])
        elif op == 39:                                  # loadb: DRAM only (the right byte
            a = r[s] & 0xFFFFFF                         # there, JaguarDocs T-GPU-11; the
            if a >= 0x200000:                           # local RAM returns the whole long)
                raise JriscError(f'loadb outside DRAM at ${self.pc - 2:06X}')
            self.poison(a)
            r[d] = self.dram[a]
        elif op == 45:                                  # storeb
            self.wr(r[s], r[d], 1)
        elif op == 47:                                  # store
            self.wr(r[s] & ~3, r[d], 4)
        elif op == 48:                                  # storep: G_HIDATA, then the register
            a = r[s] & ~7
            if a >= 0x200000:
                raise JriscError(f'storep outside DRAM at ${self.pc - 2:06X}')
            self.wr(a, self.hidata, 4)
            self.wr(a + 4, r[d], 4)
        elif op in (52, 53):                            # jump cc,(Rn) / jr cc,n
            if in_slot:
                raise JriscError(f'a jump in a delay slot at ${self.pc - 2:06X}')
            if self.cond(d):
                if op == 52:
                    t = r[s]
                else:
                    t = self.pc + 2 * (s - 32 if s & 16 else s)
                self.step(in_slot=True)                 # the delay slot
                self.pc = t
        elif op == 57:                                  # nop
            pass
        else:
            raise JriscError(f'opcode {op} at ${self.pc - 2:06X} not modelled')

    def run_until(self, stop_pc, limit=2_000_000):
        while self.pc != stop_pc:
            self.step()
            if self.steps > limit:
                raise JriscError(f'no end after {limit} instructions (pc ${self.pc:06X})')


D_RAM = 0xF1B000
D_RAM_SIZE = 0x2000
D_FLAGS = 0xF1A100


class Dsp(Gpu):
    """Jerry's DSP: 8 KB of local RAM at $F1B000, the cartridge readable, and
    the instructions src/dsp_audio.s uses on top of the GPU's (mult, sh, sha,
    sharq, sat16s, bset/bclr, the indexed loads and stores).

    No interrupts and no register banks: tools/oracle/dsp_check.py runs the
    init and then the idle loop as one program, and plays the I2S itself
    (it takes each sample at dsp_emitted and empties the ring).  Writes to
    D_FLAGS are accepted and ignored.  Every external access is checked
    against JaguarDocs' rules where an interpreter can: an external load's
    register must be READ before anything rewrites it (hw/06 rule 2).
    """

    def __init__(self, dram, code, rom=()):
        super().__init__(dram, b'')
        self.local = bytearray(D_RAM_SIZE)
        self.local[:len(code)] = code
        self.pc = D_RAM
        self.rom = list(rom)                            # [(base, bytes)]
        self.pending = None                             # an external load's register

    def rd32(self, a):
        a &= 0xFFFFFC
        if D_RAM <= a < D_RAM + D_RAM_SIZE:
            o = a - D_RAM
            return int.from_bytes(self.local[o:o + 4], 'big')
        if a < 0x200000:
            return int.from_bytes(self.dram[a:a + 4], 'big')
        for base, data in self.rom:
            if base <= a < base + len(data):
                o = a - base
                return int.from_bytes(data[o:o + 4].ljust(4, b'\0'), 'big')
        raise JriscError(f'load from ${a:06X} at ${self.pc - 2:06X}')

    def wr(self, a, v, n):
        a &= 0xFFFFFF
        if D_RAM <= a < D_RAM + D_RAM_SIZE:
            o = a - D_RAM
            self.local[o:o + n] = (v & ((1 << 8 * n) - 1)).to_bytes(n, 'big')
        elif a < 0x200000:
            self.dram[a:a + n] = (v & ((1 << 8 * n) - 1)).to_bytes(n, 'big')
        elif a == D_FLAGS:
            pass
        else:
            raise JriscError(f'store to ${a:06X} at ${self.pc - 2:06X}')

    def word(self, a):
        o = a - D_RAM
        if not 0 <= o < D_RAM_SIZE:
            raise JriscError(f'execution outside the DSP RAM: ${a:06X}')
        return int.from_bytes(self.local[o:o + 2], 'big')

    @staticmethod
    def external(a):
        a &= 0xFFFFFF
        return not (0xF00000 <= a)

    def step(self, in_slot=False):
        w = self.word(self.pc)
        op, s, d = w >> 10, (w >> 5) & 31, w & 31
        r = self.r
        # hw/06 rule 2: the destination of an external load is read before
        # it is rewritten (the barrier register is single-use).
        if self.pending is not None:
            reads = {s, d} if op not in (35, 38) else set()
            if op in (43, 44, 49, 50, 58, 59, 60, 61):
                reads |= {14 if op in (43, 49, 58, 60) else 15}
            if self.pending in reads:
                self.pending = None
            elif d == self.pending and op not in (45, 46, 47, 48, 49, 50, 60, 61, 13, 30, 31, 52, 53):
                raise JriscError(f'r{d} rewritten before its external load was used, '
                                 f'at ${self.pc:06X}')
        if op == 16:                                    # mult: low 16 x low 16
            self.pc += 2
            self.steps += 1
            r[d] = self.zn((r[s] & 0xFFFF) * (r[d] & 0xFFFF))
            return
        if op in (23, 26):                              # sh / sha by register
            self.pc += 2
            self.steps += 1
            n = r[s] - (1 << 32) if r[s] >> 31 else r[s]
            v = r[d] - (1 << 32) if (op == 26 and r[d] >> 31) else r[d]
            if not -32 <= n <= 32:
                raise JriscError(f'shift by {n} at ${self.pc - 2:06X} (verified only -32..32)')
            if n >= 32:
                res = 0
            elif n >= 0:
                res = v >> n
            elif n <= -32:
                res = 0
            else:
                res = v << -n
            r[d] = self.zn(res)
            return
        if op == 27:                                    # sharq
            self.pc += 2
            self.steps += 1
            n = s or 32
            v = r[d] - (1 << 32) if r[d] >> 31 else r[d]
            r[d] = self.zn(v >> n)
            return
        if op == 33:                                    # sat16s (DSP)
            self.pc += 2
            self.steps += 1
            v = r[d] - (1 << 32) if r[d] >> 31 else r[d]
            r[d] = self.zn(max(-32768, min(32767, v)))
            return
        if op in (14, 15):                              # bset / bclr
            self.pc += 2
            self.steps += 1
            r[d] = self.zn(r[d] | 1 << s if op == 14 else r[d] & ~(1 << s))
            return
        if op in (43, 44, 49, 50, 58, 59, 60, 61):      # indexed
            self.pc += 2
            self.steps += 1
            base = r[14] if op in (43, 49, 58, 60) else r[15]
            a = base + (4 * (s or 32) if op in (43, 44, 49, 50) else r[s])
            if op in (43, 44, 58, 59):
                if self.external(a):
                    self.pending = d
                r[d] = self.rd32(a)
            else:
                if self.external(a) and self.pending is not None:
                    raise JriscError(f'indexed store of a pending external load at ${self.pc - 2:06X}')
                self.wr(a & ~3, r[d], 4)
            return
        if op == 41:                                    # load: mark external
            a = r[s]
            super().step(in_slot)
            if self.external(a):
                self.pending = d
            return
        super().step(in_slot)
