/* The road: composers on the CPU, a command list for the GPU.
 *
 * Compiled into the Jaguar blob AND into the PC tools, like shared.h.
 *
 * The composers are skyroads-port's render.c (EXE fn_2d03 and the jump table
 * at ds:0xb7f; MIT licence, port/LICENSE-skyroads-port.txt), unchanged in
 * logic: the same records in the same order with the same colours.  What
 * changes is the output: instead of filling spans, they append commands.
 *
 * THE FRAME is ONE 320x138 8-bit buffer (the viewport), composed in the
 * original's order:
 *   1. GPU, first launch:  the pristine backdrop copied in, then every record
 *      the original draws BEFORE the ship;
 *   2. CPU:                the ship and its shadow (road_ship, road_shadow);
 *   3. GPU, second launch: the records drawn AFTER the ship (the ship row's
 *      "post-ship" set and the two nearer rows).
 * The OP shows the buffer and the dashboard over it.  (A first version
 * stacked two road layers and the ship as OP objects: three full-width
 * objects on a line stopped the real Jaguar dead - M2, hardware.)
 *
 * COMMANDS, two longs each, read by the GPU (src/gpu_road.s):
 *   (ROAD_OP_END,  0)        end of this launch's list
 *   (ROAD_OP_BASE, base)     following spans' row offsets count from base
 *   (ROAD_OP_COPY, src)      copy VIEW_BYTES from src to the current base
 *   (address, fill | half<<8) one record: address of its span block in
 *                            trekg (tools/sr2jag.py), fill byte, mirrored
 *                            if half = 1
 */
#ifndef ROAD_H
#define ROAD_H

#include "shared.h"

#define VIEW_ROWS     138
#define VIEW_BYTES    (320 * VIEW_ROWS)
#define ROAD_Y0       32          /* span row offsets count from this row */

#define ROAD_OP_END   0u
#define ROAD_OP_BASE  1u
#define ROAD_OP_COPY  4u

/* Longest list: 10 rows x 2 halves x 4 columns, 2 passes on the ship row,
 * the deepest composer draws 12 records, plus the control commands. */
#define ROAD_CMD_MAX  (2 * (11 * 2 * 4 * 12 + 16))

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
    uint32_t backdrop;        /* the pristine backdrop, GPU address */
    uint32_t *cmd;            /* out: at least ROAD_CMD_MAX longs */
    int second;               /* out: index of the second launch's list */
} road_ctx;

/* Fill c->cmd for the play state's frame (two lists, see above). */
void road_compose(road_ctx *c, const sr_play *p);

/* Step 2, on the CPU, into the frame buffer as this CPU addresses it. */
void road_ship(const road_ctx *c, uint8_t *view, const uint8_t *cars, ship_pose_t s);
void road_shadow(const road_ctx *c, uint8_t *view, const sr_play *p, ship_pose_t s);

#endif
