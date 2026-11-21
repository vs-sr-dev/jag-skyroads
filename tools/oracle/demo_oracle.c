/* The PC oracle for the attract demo.
 *
 *   demo_oracle CART_DIR
 *
 * Runs DEMO.REC on road 0 through skyroads-port's play.c - the same file the
 * Jaguar blob compiles - reading the SAME converted blobs the cartridge
 * carries (cart/roads.bin, cart/demo.bin), and writes cart/demo_expect.bin:
 *   u32 ticks, u32 final result, then DEMO_REC_BYTES per tick.
 *
 * What a match on the Jaguar proves: the 68000 build of the physics behaves
 * like the PC build, tick for tick, on the real data.  What the oracle itself
 * proves: the run COMPLETES the demo road, which skyroads-port established is
 * only possible with physics exact to the original.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/shared.h"

static uint8_t *slurp(const char *dir, const char *name, size_t *size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "demo_oracle: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);
    *size = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "cart";
    size_t nr, nd;
    uint8_t *roads = slurp(dir, "roads.bin", &nr);
    uint8_t *demo = slurp(dir, "demo.bin", &nd);

    static uint16_t grid[SR_GRID_ROWS * 7];
    static sr_road road;
    static sr_play play;
    road_load(roads, DEMO_ROAD, &road, grid);
    sr_play_init(&play, &road, DEMO_DATA(demo), DEMO_LEN(demo));

    uint8_t *recs = malloc((size_t)DEMO_MAX_TICKS * DEMO_REC_BYTES);
    int res = SR_RES_RUNNING, t = 0;
    for (; t < DEMO_MAX_TICKS && res == SR_RES_RUNNING; t++) {
        sr_input none;
        memset(&none, 0, sizeof none);
        sr_play_input(&play, &none);
        res = sr_play_tick(&play);
        demo_rec(recs + (size_t)t * DEMO_REC_BYTES, &play, res,
                 ship_pose(&play, play.tick, play.on_sticky));
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/demo_expect.bin", dir);
    FILE *f = fopen(path, "wb");
    uint8_t hdr[8];
    put32(hdr, (uint32_t)t);
    put32(hdr + 4, (uint32_t)res);
    fwrite(hdr, 1, 8, f);
    fwrite(recs, DEMO_REC_BYTES, (size_t)t, f);
    fclose(f);

    printf("demo_oracle: %d ticks (%.1f s), result %d%s, row %u of %d\n",
           t, t / 36.0036, res, res == SR_RES_COMPLETE ? " = COMPLETE" : " (NOT complete!)",
           (unsigned)(play.z >> 16), road.rows);
    return res == SR_RES_COMPLETE ? 0 : 2;
}
