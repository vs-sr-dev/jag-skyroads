/* PC-side reference for the Jaguar's frame.
 *
 * raster_run:   executes ONE road command list (csrc/road.h) with the exact
 *               semantics src/gpu_road.s implements;
 * render_view:  the whole frame buffer as the Jaguar builds it - GPU list 1,
 *               ship and shadow on the CPU, GPU list 2;
 * compose_frame: the OP's picture - the view, then the dashboard (top band
 *               transparent over rows 129..137, the rest opaque).
 *
 * Host addresses: commands carry 32-bit "GPU addresses"; here the trekg blob
 * is mapped at HOST_TREKG, the frame buffer at HOST_VIEW and the backdrop
 * at HOST_BACKDROP.
 */
#ifndef RASTER_H
#define RASTER_H

#include <string.h>
#include "../../csrc/road.h"

#define HOST_VIEW     0x00010000u
#define HOST_BACKDROP 0x00020000u
#define HOST_TREKG    0x00100000u

static inline void raster_run(const uint32_t *cmd, const uint8_t *trekg,
                              uint8_t *view, const uint8_t *backdrop)
{
    uint8_t *base = NULL;
    for (;;) {
        uint32_t a = *cmd++, b = *cmd++;
        if (a == ROAD_OP_END)
            return;
        if (a == ROAD_OP_BASE) {
            base = view + (b - HOST_VIEW);
            continue;
        }
        if (a == ROAD_OP_COPY) {
            memcpy(base, backdrop + (b - HOST_BACKDROP), VIEW_BYTES);
            continue;
        }
        const uint8_t *blk = trekg + (a - HOST_TREKG);
        uint32_t row = get32(blk), n = get32(blk + 4);
        uint8_t fill = (uint8_t)b;
        int half = (b >> 8) & 1;
        for (uint32_t i = 0; i < n; i++, row += 320) {
            uint32_t s = get32(blk + 8 + 4 * i);
            uint32_t x0 = s >> 16, len = s & 0xffff;
            uint32_t x = half ? 320 - x0 - len : x0;
            memset(base + row + x, fill, len);
        }
    }
}

static inline void render_view(road_ctx *c, const sr_play *p, uint8_t *view,
                               const uint8_t *backdrop, const uint8_t *cars,
                               ship_pose_t s)
{
    road_compose(c, p);
    raster_run(c->cmd, c->trekg, view, backdrop);
    road_ship(c, view, cars, s);
    road_shadow(c, view, p, s);
    raster_run(c->cmd + c->second, c->trekg, view, backdrop);
}

/* view: 320x138; dash: 320x71 from row 129. */
static inline void compose_frame(uint8_t *fb, const uint8_t *view, const uint8_t *dash)
{
    memset(fb, 0, 320 * 200);
    memcpy(fb, view, VIEW_BYTES);
    for (int y = 0; y < 71; y++)
        for (int x = 0; x < 320; x++) {
            uint8_t p = dash[y * 320 + x];
            int sy = 129 + y;
            if (p || sy >= 138)
                fb[sy * 320 + x] = p;
        }
}

#endif
