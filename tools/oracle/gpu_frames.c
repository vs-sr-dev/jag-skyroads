/* The GPU composers' test frames, for tools/oracle/gpu_check.py.
 *
 *   gpu_frames CART OUT [STRIDE [VIEW_EVERY]]
 *
 * For every road (0 = the demo's, 1..30) and every STRIDE-th position from
 * the start to 10 rows past the end, writes what the 68000 hands the GPU
 * (road_gpu_input's block) and what road_compose - the C original the GPU
 * code transcribes - composes from the same state.  The composers depend on
 * the road and z / 0x2000 alone, so this covers every frame the game can
 * show (STRIDE 1) or every phase on a coarser grid (STRIDE coprime with 8).
 *
 * Addresses are the Jaguar's (csrc/mem_map.h): the interpreter runs the GPU
 * on a DRAM image laid out as the cartridge does it.
 *
 * Every VIEW_EVERY-th frame (default 10) also carries the PIXELS: both
 * lists executed by raster.h - the GPU rasteriser's PC reference - over a
 * screen of 0xA5, so that the interpreter can run the GPU's rasteriser too.
 *
 * Those frames also get a ship and a shadow, at poses drawn from a fixed
 * pseudo-random sequence (edges, the cowl, off the viewport, no ship, no
 * shadow): road_ship and road_shadow_draw between the lists, which the GPU
 * does from the SHIP_IN block written here (road_ship_input_at).
 *
 * OUT, big-endian: per frame  u32 road, u32 pos, CIN_BYTES of input,
 *                             u32 second (index of list 2), u32 n,
 *                             n longs of road_compose's two lists,
 *                             u32 has_view, [SIN_BYTES of ship input,
 *                             VIEW_BYTES of screen].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/road.h"
#include "../../csrc/mem_map.h"
#include "raster.h"

static uint8_t *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("gpu_frames: no %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("gpu_frames: short read %s\n", path); exit(1); }
    fclose(f);
    return b;
}

static void wr32(FILE *f, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: gpu_frames CART OUT [STRIDE]\n"); return 1; }
    char p[1024];
    snprintf(p, sizeof p, "%s/roads.bin", argv[1]); uint8_t *roads = slurp(p);
    snprintf(p, sizeof p, "%s/trekg.bin", argv[1]); uint8_t *trekg = slurp(p);
    snprintf(p, sizeof p, "%s/tables.bin", argv[1]); uint8_t *tables = slurp(p);
    snprintf(p, sizeof p, "%s/cars.bin", argv[1]); uint8_t *cars = slurp(p);
    static uint8_t sin[SIN_BYTES];
    uint32_t seed = 0x2468ACE1u;
    int stride = argc > 3 ? atoi(argv[3]) : 1;
    int view_every = argc > 4 ? atoi(argv[4]) : 10;
    FILE *out = fopen(argv[2], "wb");
    if (!out) { printf("gpu_frames: cannot write %s\n", argv[2]); return 1; }

    static uint16_t grid[SR_GRID_ROWS * 7];
    static uint32_t cmd[ROAD_CMD_MAX];
    static uint8_t cin[CIN_BYTES];
    static uint32_t hcmd[ROAD_CMD_MAX];
    static uint8_t view[VIEW_BYTES];
    sr_road road;
    sr_play play;
    road_ctx c = { trekg, TREKG_BUF, tables, VIEW0, PRISTINE, cmd, 0 };
    road_ctx h = { trekg, HOST_TREKG, tables, HOST_VIEW, HOST_PRISTINE, hcmd, 0 };
    road_gpu_quads(cin, tables);
    long frames = 0;
    for (int e = 0; e <= 30; e++) {
        road_load(roads, e, &road, grid);
        memset(&play, 0, sizeof play);
        play.road = &road;
        uint32_t last = (uint32_t)(road.rows + 10) * 8;
        for (uint32_t pos = (uint32_t)e % (uint32_t)stride; pos <= last; pos += (uint32_t)stride) {
            play.z = pos * 0x2000u;
            road_gpu_input(cin, &play, VIEW0);
            road_compose(&c, &play);
            int n = 0;
            for (int ends = 0; ends < 2; n += 2)
                if (cmd[n] == ROAD_OP_END)
                    ends++;
            wr32(out, (uint32_t)e);
            wr32(out, pos);
            fwrite(cin, 1, CIN_BYTES, out);
            wr32(out, (uint32_t)c.second);
            wr32(out, (uint32_t)n);
            for (int i = 0; i < n; i++)
                wr32(out, cmd[i]);
            int has_view = view_every > 0 && frames % view_every == 0;
            wr32(out, (uint32_t)has_view);
            if (has_view) {
                ship_pose_t s;
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                s.frame = (int16_t)(seed % 16 == 0 ? -1 : (int)(seed % 77));
                s.top = (int16_t)((int)((seed >> 8) % 175) - 30);
                s.left = (int16_t)((int)((seed >> 16) % 360) - 35);
                int shadow = (seed >> 24) % 6 != 0 && s.frame >= 0;
                int map = (int)((seed >> 4) % 5);
                int stop = s.top + 0x10 + (int)((seed >> 20) % 24);
                road_ship_input_at(sin, &c, s, shadow, map, stop, s.left, CARS_BUF, SHADOW_MASKS);
                fwrite(sin, 1, sizeof sin, out);
                road_compose(&h, &play);
                memset(view, 0xA5, sizeof view);
                raster_run(h.cmd, trekg, view, NULL);
                road_ship(&h, view, cars, s);
                if (shadow)
                    road_shadow_draw(&h, view, map, stop, s.left);
                raster_run(h.cmd + h.second, trekg, view, NULL);
                fwrite(view, 1, sizeof view, out);
            }
            frames++;
        }
    }
    fclose(out);
    printf("gpu_frames: %ld frames, stride %d\n", frames, stride);
    return 0;
}
