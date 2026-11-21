/* PC-side reference for the Jaguar's frame.
 *
 * raster_run:   executes ONE road command list (csrc/road.h) with the exact
 *               semantics src/gpu_road.s implements;
 * render_view:  the whole 320x200 frame as the Jaguar builds it - the
 *               Blitter's viewport copy, GPU list 1, ship and shadow on the
 *               CPU, GPU list 2, gauges incrementally on the CPU.
 *
 * Host addresses: commands carry 32-bit "GPU addresses"; here the trekg blob
 * is mapped at HOST_TREKG, the frame buffer at HOST_VIEW and the backdrop
 * at HOST_BACKDROP.
 */
#ifndef RASTER_H
#define RASTER_H

#include <string.h>
#include "../../csrc/hud.h"

#define HOST_VIEW     0x00010000u
#define HOST_PRISTINE 0x00020000u
#define HOST_TREKG    0x00100000u

static inline void raster_run(const uint32_t *cmd, const uint8_t *trekg,
                              uint8_t *view, const uint8_t *pristine)
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
            memcpy(base, pristine + (b - HOST_PRISTINE), VIEW_BYTES);
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
                               const uint8_t *pristine, const uint8_t *cars,
                               const uint8_t *hud, ship_pose_t s, hud_state *st)
{
    memcpy(view, pristine, VIEW_COPY_BYTES);          /* the Blitter's copy */
    road_compose(c, p);
    raster_run(c->cmd, c->trekg, view, pristine);
    road_ship(c, view, cars, s);
    road_shadow(c, view, p, s);
    raster_run(c->cmd + c->second, c->trekg, view, pristine);
    hud_update(view, pristine, hud, p, st);
}

#endif
