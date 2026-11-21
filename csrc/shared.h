/* Code compiled BOTH into the Jaguar blob and into the PC oracle
 * (tools/oracle/demo_oracle.c).  One copy, included by both: the oracle's
 * expectations can only disagree with the cartridge through the compiler or
 * the machine, never through a second transcription.
 *
 * ship_pose: which CARS frame the ship shows and where, after a tick.  The
 * logic is skyroads-port's render.c (sr_render_frame, sprite selection, from
 * EXE fn_0be3), MIT licence - see port/LICENSE-skyroads-port.txt.
 *
 * demo_rec: the per-tick record the attract demo is checked with.
 */
#ifndef SHARED_H
#define SHARED_H

#include "port/play.h"

/* ds:0x38: the sprite's sideways nudge per tilt class. */
static const int16_t ship_pan[7] = { -1, -1, -1, 0, 1, 2, 4 };

typedef struct {
    int16_t frame;      /* CARS frame 0..76, -1 = nothing (explosion over) */
    int16_t left;       /* screen x of the sprite's first column */
    int16_t top;        /* screen y of the sprite's first row */
} ship_pose_t;

static inline ship_pose_t ship_pose(const sr_play *p, uint32_t tick, int on_sticky)
{
    ship_pose_t s;
    int in_tun = sr_in_tunnel(p, p->z, p->x, p->y);
    int tilt = ((int)(p->x / 0x80) - 95) / 46;
    if (tilt < 0) tilt = 0;
    if (tilt > 6) tilt = 6;
    if (p->expl_ctr) {
        s.frame = (int16_t)(p->expl_ctr / 3);
        if (s.frame >= 14)
            s.frame = -1;
    } else {
        int pitch = 0;
        if (!in_tun) {
            if (p->yvel <= -0x163 || p->y < 0x2800) pitch = 2;
            else if (p->yvel >= 0x163) pitch = 1;
        }
        static const int flame_tab[4] = { 0, 1, 2, 1 };
        int flame = (p->end_state == SR_RES_NO_FUEL) ? 0 : flame_tab[(tick / 2) & 3];
        s.frame = (int16_t)(14 + 9 * tilt + 3 * pitch + flame);
    }
    int alt = ((int)(on_sticky ? p->y - 0x80 : p->y)) / 0x80;
    int ship_x = (int)(p->x / 0x80) + ship_pan[tilt];
    s.left = (int16_t)(ship_x - 0x6e);
    s.top = (int16_t)(0x9d - alt);
    return s;
}

/* The record: big-endian, written byte by byte so that struct layout (the
 * 68000 aligns longs to 2, the PC to 4) cannot enter the comparison. */
#define DEMO_REC_BYTES 32

static inline void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static inline void put32(uint8_t *b, uint32_t v) { put16(b, (uint16_t)(v >> 16)); put16(b + 2, (uint16_t)v); }

static inline void demo_rec(uint8_t *b, const sr_play *p, int res, ship_pose_t s)
{
    put32(b + 0, p->z);
    put16(b + 4, p->x);
    put16(b + 6, p->y);
    put32(b + 8, (uint32_t)p->speed);
    put16(b + 12, (uint16_t)p->yvel);
    put16(b + 14, (uint16_t)p->xvel);
    put16(b + 16, (uint16_t)p->fuel);
    put16(b + 18, (uint16_t)p->oxy);
    put16(b + 20, (uint16_t)p->end_state);
    put16(b + 22, (uint16_t)p->expl_ctr);
    put16(b + 24, (uint16_t)res);
    put16(b + 26, (uint16_t)s.frame);
    put16(b + 28, (uint16_t)s.left);
    put16(b + 30, (uint16_t)s.top);
}

static inline uint16_t get16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static inline uint32_t get32(const uint8_t *b) { return ((uint32_t)get16(b) << 16) | get16(b + 2); }

/* A road out of roads.bin (tools/sr2jag.py): an index of 16-byte entries
 * {u32 offset, u16 rows, u16 gravity, u16 word2, u16 word3, u32 0}, then the
 * rows of 7 big-endian u16.  grid must hold SR_GRID_ROWS*7 cells: the
 * original clears a 500-row buffer and the physics reads past the end of
 * short roads into those zeros. */
#define SR_GRID_ROWS 500

static inline void road_load(const uint8_t *roads, int entry, sr_road *r, uint16_t *grid)
{
    const uint8_t *ix = roads + 16 * entry;
    const uint8_t *src = roads + get32(ix);
    r->rows = get16(ix + 4);
    r->gravity = get16(ix + 6);
    r->word2 = get16(ix + 8);
    r->word3 = get16(ix + 10);
    r->cells = grid;
    for (int i = 0; i < SR_GRID_ROWS * 7; i++)
        grid[i] = i < r->rows * 7 ? get16(src + 2 * i) : 0;
}

/* demo.bin: u32 length, then DEMO.REC. */
#define DEMO_LEN(blob)   get32(blob)
#define DEMO_DATA(blob)  ((blob) + 4)

/* The demo as the game runs it: road 0, DEMO.REC as input. */
#define DEMO_ROAD 0
#define DEMO_MAX_TICKS (36 * 240)

#endif
