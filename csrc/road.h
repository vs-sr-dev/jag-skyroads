/* The road: composers on the CPU, a command list for the GPU.
 *
 * Compiled into the Jaguar blob AND into the PC tools, like shared.h.
 *
 * The composers are skyroads-port's render.c (EXE fn_2d03 and the jump table
 * at ds:0xb7f; MIT licence, port/LICENSE-skyroads-port.txt), unchanged in
 * logic: the same records in the same order with the same colours.  What
 * changes is the output: instead of filling spans, they append commands.
 *
 * THE FRAME is ONE 320x200 8-bit buffer, the whole game screen, composed
 * in the original's order:
 *   0. Blitter:            the pristine viewport (rows 0..137 of the screen
 *      hud_pristine builds at road start) copied in, one phrase-mode blit;
 *   1. GPU, first launch:  the composers (src/gpu_road.s, a transcription of
 *      road_compose below) write the WHOLE command list, then, once the
 *      Blitter is done, every record the original draws BEFORE the ship;
 *   2. CPU:                the ship and its shadow (road_ship, road_shadow);
 *   3. GPU, second launch: the records drawn AFTER the ship (the ship row's
 *      "post-ship" set and the two nearer rows);
 *   4. CPU:                the gauges, incrementally (hud_update).
 * The OP shows the buffer as ONE object.  (A first version stacked road
 * layers and the ship as OP objects: three full-width objects on a line
 * stopped the real Jaguar dead - M2, hardware.)
 *
 * road_compose is the C original of the GPU's composers, kept as the
 * reference: the PC renders with it (tools/oracle/raster.h), the PC checks
 * the GPU's code against it in an interpreter (tools/oracle/gpu_check.py),
 * and the 68000 checks the GPU against it on the keypad (csrc/game_main.c).
 *
 * The GPU reads external memory only as aligned longs, so it does not read
 * the road grid (u16 cells, play.c's): the CPU hands it, every frame, the
 * eleven grid rows a frame looks at, one long per cell (road_gpu_input), in
 * a block whose layout is the CIN_* offsets below.
 *
 * COMMANDS, two longs each, read by the GPU (src/gpu_road.s):
 *   (ROAD_OP_END,  0)        end of this launch's list
 *   (ROAD_OP_BASE, base)     following spans' row offsets count from base
 *   (ROAD_OP_COPY, src)      copy VIEW_BYTES from src to the current base
 *                            (unused since the Blitter does it; kept in the
 *                            GPU as a fallback)
 *   (address, fill | half<<8) one record: address of its span block in
 *                            trekg (tools/sr2jag.py), fill byte, mirrored
 *                            if half = 1
 */
#ifndef ROAD_H
#define ROAD_H

#include "shared.h"

#define VIEW_ROWS     200
#define VIEW_BYTES    (320 * VIEW_ROWS)
/* What is restored from the pristine screen every frame: the viewport and
 * the cowl band over it (rows 0..137).  The dashboard below persists in each
 * frame buffer and is kept up to date by hud_update. */
#define VIEW_COPY_ROWS  138
#define VIEW_COPY_BYTES (320 * VIEW_COPY_ROWS)
#define ROAD_Y0       32          /* span row offsets count from this row */

#define ROAD_OP_END   0u
#define ROAD_OP_BASE  1u
#define ROAD_OP_COPY  4u

/* Longest list: 10 rows x 2 halves x 4 columns, 2 passes on the ship row,
 * the deepest composer draws 12 records, plus the control commands. */
#define ROAD_CMD_MAX  (2 * (11 * 2 * 4 * 12 + 16))

/* The GPU's input block (COMP_IN in tools/memory_map.py), big-endian longs:
 *   CIN_POS    z / 0x2000: the phase object is pos & 7, the base row pos >> 3
 *   CIN_BASE   the frame buffer's address + ROAD_Y0 rows (ROAD_OP_BASE)
 *   CIN_QUAD0  74 longs: the colour quads' byte 0, the left half's fill
 *   CIN_QUAD1  74 longs: byte 1, the right half's fill
 *   CIN_WIN    CIN_ROWS rows of 8 longs: row w is grid row (pos >> 3) - 3 + w,
 *              columns 0..6 and a zero; rows outside the grid are zeros.
 *              A pass over grid row g reads rows g and g - 1 (the nearer
 *              one), and the inner neighbour never leaves columns 0..6. */
#define CIN_POS     0
#define CIN_BASE    4
#define CIN_QUAD0   16
#define CIN_QUAD1   (CIN_QUAD0 + 4 * 74)
#define CIN_WIN     (CIN_QUAD1 + 4 * 74)
#define CIN_ROWS    11
#define CIN_BYTES   (CIN_WIN + CIN_ROWS * 8 * 4)

/* tables.bin (tools/sr2jag.py, from SKYROADS.EXE's data segment). */
#define TAB_QUAD      0           /* ds:0x322, 74 x 4 bytes */
#define TAB_SHADOW    296         /* ds:0x65e, 5 x 261 bytes */
#define TAB_COWL      1608        /* ds:0x44a, 138 x u16 BE */

/* cars.bin: 77 frames of 32x24, row-major (tools/sr2jag.py). */
#define CAR_FRAME_BYTES 768

typedef struct {
    const uint8_t *trekg;     /* the blob as this CPU reads it */
    uint32_t trekg_at;        /* the blob's address as the GPU sees it */
    const uint8_t *tables;
    uint32_t view;            /* the frame buffer, GPU address */
    uint32_t pristine;        /* the pristine screen, GPU address */
    uint32_t *cmd;            /* out: at least ROAD_CMD_MAX longs */
    int second;               /* out: index of the second launch's list */
} road_ctx;

/* Fill c->cmd for the play state's frame (two lists, see above). */
void road_compose(road_ctx *c, const sr_play *p);

/* The GPU's input block: road_gpu_quads once (the tables never change),
 * road_gpu_input every frame. */
void road_gpu_quads(uint8_t *cin, const uint8_t *tables);
void road_gpu_input(uint8_t *cin, const sr_play *p, uint32_t view);

/* Step 2, on the CPU, into the frame buffer as this CPU addresses it. */
void road_ship(const road_ctx *c, uint8_t *view, const uint8_t *cars, ship_pose_t s);
void road_shadow(const road_ctx *c, uint8_t *view, const sr_play *p, ship_pose_t s);

#endif
