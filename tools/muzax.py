#!/usr/bin/env python3
"""MUZAX, SkyRoads' AdLib driver, run at BUILD time: songs -> OPL2 register streams.

The original plays MUZAX.LZS through a driver called at 180.02 Hz (EXE
0x5a39) that turns a stream of 16-bit events into OPL2 register writes.
Nothing it does depends on the game: it reads only the song.  So the same
driver runs here, once, and the cartridge carries what it would have written
to port 0x388, tick by tick; Jerry plays that through an OPL2 synthesised on
the DSP (src/dsp_audio.s).

Written from the disassembly (SKYROADS.EXE; skyroads-port's
re/notes/audio_misc.md section 2 quotes the instructions), and compared with
skyroads-port's audio.c by tools/oracle/muzax_check.c: the two must emit the
same writes in the same ticks.

THE STREAM: big-endian 16-bit entries, two per long, high half first.
    $RRVV   write VV to OPL register RR ($01..$F5)
    $00NN   wait NN ticks (1..255) before the next entry; $0000 = nothing
    $FF00   loop: go on, in the same tick, from the song's loop point
Every stream and every loop point starts on a long (the DSP reads longs).

What the original does around a song, kept here:
  - music_start (0x57a8) RESETS the chip (0x5889: every TL to $3F, key off
    7..0) and points the driver at its idle stream (ds:0xcbc: set_loop,
    delay 255, goto_loop) while the song is read and decompressed.  The first
    tick after it arms the 255-tick delay, and the song's own events start
    only when it runs out: 255 ticks (1.42 s) after the reset, whatever the
    load took.  A song stream is: the reset writes, wait 255, the song.
  - the driver's boot init (0x58b1) - wave select, rhythm mode, the four
    default percussion patches (ds:0xc90), their fixed pitches (0x58df) -
    is the INIT stream the DSP plays once at power-up.
"""
import struct

# Driver tables (EXE data segment; audio_misc.md 2.4-2.6).
REG_GROUP = [0x20, 0x40, 0x60, 0x80, 0xE0, 0x20, 0x40, 0x60, 0x80, 0xE0, 0xC0]   # ds:0xc1c
OP1 = [0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x14, 0x12, 0x15, 0x11]         # ds:0xc27
OP2 = [0x03, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x13, 0xFF, 0xFF, 0xFF, 0xFF]         # ds:0xc32
C0IDX = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xFF, 0x08, 0xFF]       # ds:0xc3d
VOLTAB = [0x3F, 0x14, 0x10, 0x0E, 0x0C, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x06, 0x05, 0x05,
          0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x02, 0x02, 0x02, 0x01,
          0x01, 0x01, 0x01, 0x00, 0x00]                                           # ds:0xc48
FNUM_LO = [0xAC, 0xB6, 0xC1, 0xCD, 0xD9, 0xE6, 0xF3, 0x02, 0x11, 0x22, 0x33, 0x45]  # ds:0xc77
FNUM_HI = [0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1]                                    # ds:0xc83
PERC_PATCH = bytes.fromhex(                                                       # ds:0xc90
    '0c00f8b500' '0000d64f00' '01'
    '0400f7b500' '0000d64f00' '01'
    '0100f5b500' '0000d64f00' '01'
    '0100f7b500' '4e00100000' '01')

IDLE_DELAY = 255          # ds:0xcbc: 06 00 | 00 ff | 05 00
LOOP = 0xFF00


class Driver:
    """The driver's state and its register-writing routines, as in the EXE."""

    def __init__(self):
        self.out = []                   # register writes of the current tick
        self.bd = 0xE0                  # [0x31a6], reg $BD's shadow
        self.chan_instr = [0] * 11      # [0x31a7..]
        self.instr = b''                # [0x31a0]: the instrument records

    def w(self, reg, val):              # 0x5876
        self.out.append((reg & 0xFF) << 8 | (val & 0xFF))

    def key_off(self, ch):              # 0x59b3
        if ch < 6:
            self.w(0xB0 + ch, 0)
        else:
            self.bd &= ~(0x10 >> (ch - 6)) & 0xFF
            self.w(0xBD, self.bd)

    def reset(self):                    # 0x5889
        self.bd = 0xE0
        for r in range(0x40, 0x56):
            self.w(r, 0x3F)
        for ch in range(7, -1, -1):
            self.key_off(ch)

    def program(self, ch, n, base):     # 0x58fd: record n at base (16 bytes each)
        self.key_off(ch)
        rec = base[n * 16:n * 16 + 11]
        self.chan_instr[ch] = n
        for b in range(5):
            self.w(OP1[ch] + REG_GROUP[b], rec[b])
        if OP2[ch] != 0xFF:
            for b in range(5, 10):
                self.w(OP2[ch] + REG_GROUP[b], rec[b])
        if C0IDX[ch] != 0xFF:
            self.w(0xC0 + C0IDX[ch], rec[10])

    def note_on(self, ch, note):        # 0x5955
        self.key_off(ch)
        if ch < 7:
            phys = C0IDX[ch]
            block, semi = note // 12 + 2, note % 12
            self.w(0xA0 + phys, FNUM_LO[semi])
            b = (FNUM_HI[semi] | (block << 2)) & 0xFF
            if 0xB0 + phys < 0xB6:
                b |= 0x20
            self.w(0xB0 + phys, b)
            if ch < 6:
                return
        self.bd |= 0x10 >> (ch - 6)
        self.w(0xBD, self.bd)

    def volume(self, ch, vol):          # 0x59f1
        rec = self.instr[self.chan_instr[ch] * 16:]
        if OP2[ch] == 0xFF:
            ops = [(OP1[ch], rec[1])]
        else:
            ops = [(OP2[ch], rec[6])]
            if rec[10] & 1:
                ops.append((OP1[ch], rec[1]))
        for op, v in ops:               # 0x59d3
            att = (v & 0x3F) + VOLTAB[vol]
            if att > 0x3F:
                att = 0x3F
            self.w(0x40 + op, (v & 0xC0) | att)

    def state(self):
        return (self.bd, tuple(self.chan_instr))


def init_writes():
    """0x58b1, once at boot: the writes in order."""
    d = Driver()
    d.reset()
    d.w(0x01, 0x20)
    d.w(0x08, 0x00)
    d.w(0xBD, 0xE0)
    for ch in range(7, 11):
        base = PERC_PATCH[(ch - 7) * 11:]
        d.instr = base
        d.program(ch, 0, base)
    d.w(0xA8, 0xAC)
    d.w(0xB8, 0x0C)
    d.w(0xA7, 0x02)
    d.w(0xB7, 0x0D)
    return d.out


def song_entries(song, n_instr, max_passes=4):
    """Run the driver over one song: (entries, loop index).

    A flat list of stream entries (register writes and waits, as the driver
    would have produced them tick by tick) ending where the song repeats.
    The writes depend on the driver's state, so the loop is exact only where
    the position AND the state repeat: every entry into the loop body (at
    set_loop, and after each goto_loop) is marked with both, and the stream
    ends at the first goto_loop that lands on a mark.  The body is unrolled
    until it does (never, in the retail songs: checked by the build).
    """
    d = Driver()
    d.instr = song
    ev0 = n_instr * 16
    words = struct.unpack_from('<%dH' % ((len(song) - ev0) // 2), song, ev0)
    entries = []
    marks = {}
    loop_pos = 0
    marks[(0, d.state())] = 0           # 0x5a61: the loop point = the start
    pos, passes = 0, 0
    while True:
        w = words[pos]
        pos += 1
        op, ch, p = w & 7, (w >> 4) & 15, w >> 8
        if op == 0:
            entries += d.out
            d.out = []
            if p:                       # delay 0: no wait at all
                entries.append(p)
        elif op == 1:
            d.program(ch, p, song)
        elif op == 2:
            d.note_on(ch, p)
        elif op == 3:
            d.key_off(ch)
        elif op == 4:
            d.volume(ch, p)
        elif op == 5:                   # goto loop, in the same tick
            entries += d.out
            d.out = []
            key = (loop_pos, d.state())
            if key in marks:
                return entries, marks[key]
            passes += 1
            if passes > max_passes:
                raise SystemExit('muzax: a loop that never settles')
            marks[key] = len(entries)
            pos = loop_pos
        elif op == 6:                   # set loop point
            entries += d.out
            d.out = []
            loop_pos = pos
            marks[(pos, d.state())] = len(entries)
        elif op == 7:
            pass                        # sync flag, never read


def encode(entries):
    """16-bit entries -> bytes, padded to a whole long."""
    if len(entries) & 1:
        entries = entries + [0]
    return struct.pack('>%dH' % len(entries), *entries)


def song_stream(song, n_instr):
    """The song's stream (reset, the idle wait, the song) and its loop offset in bytes."""
    body, loop_at = song_entries(song, n_instr)
    d = Driver()
    d.reset()
    head = d.out + [IDLE_DELAY]
    entries = head + body
    loop_at += len(head)
    if loop_at & 1:                     # the loop point on a long
        entries = entries[:loop_at] + [0] + entries[loop_at:]
        loop_at += 1
    entries.append(LOOP)
    return encode(entries), loop_at * 2


def ticks_of(stream_bytes, loop_ofs, n_ticks):
    """Expand a stream into per-tick write lists, with the driver's counter.

    A wait N at tick T resumes at T + N: the counter is set to N and the
    same tick already takes one off (0x5a3c-0x5a43).
    """
    e = struct.unpack('>%dH' % (len(stream_bytes) // 2), stream_bytes)
    out, i, c = [], 0, 0
    for _ in range(n_ticks):
        cur = []
        if c:
            c -= 1
        else:
            while True:
                v = e[i]
                i += 1
                if v == LOOP:
                    i = loop_ofs // 2
                elif v >> 8 == 0:
                    if v:
                        c = v - 1
                        break
                else:
                    cur.append(v)
        out.append(cur)
    return out


def load_songs(muzax):
    """MUZAX.LZS -> [(song bytes, n_instr)], from its 6-byte directory."""
    import lzs
    songs = []
    for n in range(20):
        off, n_instr, raw = struct.unpack_from('<HHH', muzax, n * 6)
        if raw == 0:
            continue
        songs.append((lzs.decompress(lzs.Stream(muzax, off), raw), n_instr))
    return songs


def stream_with_idle(writes):
    """Writes in one tick, then the idle wait for ever (ds:0xcbc)."""
    entries = list(writes)
    if len(entries) & 1:
        entries.append(0)
    loop_at = len(entries)
    entries += [IDLE_DELAY, LOOP]
    return encode(entries), loop_at * 2


def build_blob(muzax_lzs):
    """cart/muzax.bin: the streams Jerry plays (src/dsp_audio.s).

        u32 n                      streams: INIT, song 0..13, SILENCE
        u32 start, loop  [n]       byte offsets from the blob's start
        streams                    each on a long

    INIT is the driver's boot init (0x58b1), played once at power-up;
    SILENCE is music_start's reset with no song after it (sound off).
    """
    streams = [stream_with_idle(init_writes())]
    for song, n_instr in load_songs(muzax_lzs):
        streams.append(song_stream(song, n_instr))
    d = Driver()
    d.reset()
    streams.append(stream_with_idle(d.out))
    head = 4 + 8 * len(streams)
    out = bytearray(struct.pack('>I', len(streams)))
    ofs = head
    table, data = bytearray(), bytearray()
    for s, loop in streams:
        table += struct.pack('>II', ofs, ofs + loop)
        data += s
        ofs += len(s)
    return bytes(out + table + data)


STREAM_INIT, STREAM_SONG0, STREAM_SILENCE = 0, 1, 15
