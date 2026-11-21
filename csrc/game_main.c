/* JagSkyroads - the game logic on the 68000.  M1: the attract demo.
 *
 * DEMO.REC drives road 0 through skyroads-port's play.c, one call of c_tick
 * per 36.0036 Hz game tick.  After every tick the state is packed into the
 * same record the PC oracle wrote (csrc/shared.h) and compared with its table
 * in the cartridge (ROM_DEMOX).  The first tick that differs is kept in
 * DEMO_BAD; RMAC shows it through the BG probe.
 *
 * The boundary with RMAC is memory cells, not calls: the ship's pose goes to
 * SHIP_FRAME/LEFT/TOP and RMAC turns it into an OP object.
 */
#include <string.h>
#include "shared.h"
#include "mem_map.h"

#define HW16(a) (*(volatile int16_t *)(uintptr_t)(a))
#define HW32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* Ticks the demo holds its last frame before starting again. */
#define DEMO_PAUSE 72

static uint16_t grid[SR_GRID_ROWS * 7];
static sr_road road;
static sr_play play;
static int result;
static uint32_t pause;

static void publish(ship_pose_t s)
{
    HW16(SHIP_FRAME) = s.frame;
    HW16(SHIP_LEFT) = s.left;
    HW16(SHIP_TOP) = s.top;
}

static void demo_start(void)
{
    const uint8_t *demo = (const uint8_t *)(uintptr_t)ROM_DEMO;
    road_load((const uint8_t *)(uintptr_t)ROM_ROADS, DEMO_ROAD, &road, grid);
    sr_play_init(&play, &road, DEMO_DATA(demo), DEMO_LEN(demo));
    result = SR_RES_RUNNING;
    pause = 0;
    HW32(DEMO_TICK) = 0;
    publish(ship_pose(&play, play.tick, play.on_sticky));
}

static void mismatch(uint32_t t)
{
    if (HW32(DEMO_BAD) == 0)
        HW32(DEMO_BAD) = t + 1;
}

uint32_t c_init(void)
{
    HW32(DEMO_BAD) = 0;
    HW32(DEMO_RUNS) = 0;
    demo_start();
    return C_MAGIC;
}

void c_tick(void)
{
    const uint8_t *x = (const uint8_t *)(uintptr_t)ROM_DEMOX;
    uint32_t n_exp = get32(x);
    uint32_t t = HW32(DEMO_TICK);

    if (result != SR_RES_RUNNING) {
        if (++pause >= DEMO_PAUSE)
            demo_start();
        return;
    }

    sr_input none;
    memset(&none, 0, sizeof none);
    sr_play_input(&play, &none);
    result = sr_play_tick(&play);
    ship_pose_t s = ship_pose(&play, play.tick, play.on_sticky);
    publish(s);

    uint8_t rec[DEMO_REC_BYTES];
    demo_rec(rec, &play, result, s);
    if (t < n_exp) {
        const uint8_t *e = x + 8 + t * DEMO_REC_BYTES;
        for (int i = 0; i < DEMO_REC_BYTES; i++)
            if (rec[i] != e[i]) {
                mismatch(t);
                break;
            }
    } else {
        mismatch(t);                /* the oracle's run had already ended */
    }
    t++;
    HW32(DEMO_TICK) = t;

    if (result != SR_RES_RUNNING) {
        if (t != n_exp || (uint32_t)result != get32(x + 4))
            mismatch(t);
        else if (HW32(DEMO_BAD) == 0)
            HW32(DEMO_RUNS) = HW32(DEMO_RUNS) + 1;
    }
}
