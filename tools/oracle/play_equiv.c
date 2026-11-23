/* csrc/port/play.c (ours, optimised for the 68000) against skyroads-port's
 * play.c as frozen in ref/ (the reference): the same physics, bit for bit?
 *
 *   play_equiv CART_DIR [RUNS]
 *
 * The reference is compiled from ref/skyroads-port-main/src/core/play.c with
 * its symbols renamed ref_* (tools/oracle/play_equiv.sh shows the command;
 * build.bat runs it).  Then, for every road:
 *   1. the pure queries (sr_tile_at, sr_solid, sr_in_tunnel) on random and
 *      on edge arguments: every answer equal;
 *   2. RUNS games with pseudo-random held inputs (steering, throttle, jumps
 *      changing every few ticks), the two states side by side: after EVERY
 *      tick the whole sr_play struct and the result must be equal;
 * and the attract demo (road 0 with DEMO.REC) to its end.
 * Exit 1 at the first difference, with where it happened.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/shared.h"

void ref_sr_play_init(sr_play *p, const sr_road *road, const uint8_t *demo, size_t demo_size);
int ref_sr_play_tick(sr_play *p);
void ref_sr_play_input(sr_play *p, const sr_input *in);
uint16_t ref_sr_tile_at(const sr_play *p, uint32_t z, uint16_t x);
int ref_sr_solid(const sr_play *p, uint32_t z, uint16_t x, uint16_t y);
int ref_sr_in_tunnel(const sr_play *p, uint32_t z, uint16_t x, uint16_t y);

static uint8_t *slurp(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "play_equiv: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);
    return b;
}

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fail(const char *what, int road, long run, long tick)
{
    printf("play_equiv: DIFFERENT - %s, road %d run %ld tick %ld\n", what, road, run, tick);
    exit(1);
}

/* Interesting values for y and x: decks, block tops, tunnel rows, wraps. */
static uint16_t pick_y(void)
{
    static const uint16_t ys[] = { 0, 1, 0x2200, 0x2480, 0x2800, 0x27ff, 0x2801, 0x3200, 0x31ff,
                                   0x3c00, 0x3bff, 0x3700, 0x4000, 0x7fff, 0x8000, 0xffff };
    return (rnd() & 1) ? ys[rnd() % (sizeof ys / sizeof ys[0])] : (uint16_t)rnd();
}

static uint16_t pick_x(void)
{
    uint32_t r = rnd() % 4;
    if (r == 0)
        return (uint16_t)rnd();
    if (r == 1)
        return (uint16_t)((rnd() % 512) * 0x80 + (rnd() & 1 ? 0 : 0x7f));
    return (uint16_t)(0x2f80 + rnd() % (0xd080 - 0x2f80 + 1));
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "cart";
    long runs = argc > 2 ? atol(argv[2]) : 40;
    uint8_t *roads = slurp(dir, "roads.bin");
    uint8_t *demo = slurp(dir, "demo.bin");
    static uint16_t grid[SR_GRID_ROWS * 7];
    static sr_road road;
    static sr_play a, b;
    long queries = 0, ticks = 0;

    for (int e = 0; e <= 30; e++) {
        road_load(roads, e, &road, grid);
        sr_play_init(&a, &road, NULL, 0);
        ref_sr_play_init(&b, &road, NULL, 0);
        if (memcmp(&a, &b, sizeof a))
            fail("init", e, 0, 0);

        /* 1. the pure queries */
        for (long i = 0; i < 400000; i++) {
            uint32_t z = (rnd() & 3) ? (uint32_t)(rnd() % ((uint32_t)(road.rows + 8) << 16)) : rnd();
            uint16_t x = pick_x(), y = pick_y();
            if (sr_tile_at(&a, z, x) != ref_sr_tile_at(&b, z, x))
                fail("sr_tile_at", e, -1, i);
            if (sr_solid(&a, z, x, y) != ref_sr_solid(&b, z, x, y))
                fail("sr_solid", e, -1, i);
            if (sr_in_tunnel(&a, z, x, y) != ref_sr_in_tunnel(&b, z, x, y))
                fail("sr_in_tunnel", e, -1, i);
            queries++;
        }

        /* 2. one tick from random ship states: random games die early (they
         * fall), so walls, block sides and row boundaries are rarely met
         * there; here every branch of swept_move is, many times. */
        for (long i = 0; i < 200000; i++) {
            sr_play_init(&a, &road, NULL, 0);
            a.z = rnd() % ((uint32_t)road.rows << 16);
            a.x = (rnd() & 7) ? (uint16_t)(0x2f80 + rnd() % (0xd080 - 0x2f80 + 1)) : (uint16_t)rnd();
            a.y = (rnd() & 1) ? pick_y() : (uint16_t)(0x2000 + rnd() % 0x3000);
            a.last_new_y = (rnd() & 1) ? a.y : pick_y();
            a.speed = (int32_t)(rnd() % 0x2aab);
            a.yvel = (int16_t)((int32_t)(rnd() % 0x1000) - 0x800);
            a.xvel = (int16_t)(((int32_t)(rnd() % 5) - 2) * 0x1d);
            a.side_push = (int16_t)(((int32_t)(rnd() % 7) - 3) * 0x11);
            a.on_ground = (int16_t)(rnd() & 1);
            a.jumping = (int16_t)(rnd() & 1);
            a.jump_start_y = pick_y();
            a.steer = (int16_t)((int32_t)(rnd() % 3) - 1);
            a.accel = (int16_t)((int32_t)(rnd() % 3) - 1);
            a.jumpkey = (int16_t)(rnd() & 1);
            a.edge_dist = (int16_t)(rnd() % 16);
            b = a;
            int ra = sr_play_tick(&a), rb = ref_sr_play_tick(&b);
            ticks++;
            if (ra != rb || memcmp(&a, &b, sizeof a))
                fail("tick from a random state", e, -2, i);
        }

        /* 3. games with random inputs, state compared every tick */
        for (long run = 0; run < runs; run++) {
            sr_play_init(&a, &road, NULL, 0);
            ref_sr_play_init(&b, &road, NULL, 0);
            sr_input in;
            memset(&in, 0, sizeof in);
            int hold = 0;
            for (long t = 0; t < 6000; t++) {
                if (hold-- <= 0) {
                    uint32_t r = rnd();
                    memset(&in, 0, sizeof in);
                    in.held[SR_KEY_UP] = (r & 7) < 5;
                    in.held[SR_KEY_DOWN] = (r & 7) == 7;
                    in.held[SR_KEY_LEFT] = ((r >> 3) & 3) == 1;
                    in.held[SR_KEY_RIGHT] = ((r >> 3) & 3) == 2;
                    in.held[SR_KEY_JUMP] = ((r >> 5) & 3) == 0;
                    hold = (int)((r >> 8) % 24);
                }
                sr_play_input(&a, &in);
                ref_sr_play_input(&b, &in);
                int ra = sr_play_tick(&a), rb = ref_sr_play_tick(&b);
                ticks++;
                if (ra != rb || memcmp(&a, &b, sizeof a))
                    fail("game state", e, run, t);
                if (ra != SR_RES_RUNNING)
                    break;
            }
        }
    }

    /* 4. the attract demo, to its end */
    road_load(roads, DEMO_ROAD, &road, grid);
    sr_play_init(&a, &road, DEMO_DATA(demo), DEMO_LEN(demo));
    ref_sr_play_init(&b, &road, DEMO_DATA(demo), DEMO_LEN(demo));
    sr_input none;
    memset(&none, 0, sizeof none);
    long t = 0;
    for (int r = SR_RES_RUNNING; r == SR_RES_RUNNING; t++) {
        sr_play_input(&a, &none);
        ref_sr_play_input(&b, &none);
        r = sr_play_tick(&a);
        if (r != ref_sr_play_tick(&b) || memcmp(&a, &b, sizeof a))
            fail("attract demo", DEMO_ROAD, 0, t);
        ticks++;
    }
    printf("play_equiv: identical - %ld queries, %ld ticks over 31 roads and the demo (%ld ticks)\n",
           queries, ticks, t);
    return 0;
}
