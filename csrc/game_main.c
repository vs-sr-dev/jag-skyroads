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
#include "road.h"
#include "mem_map.h"

#define HW16(a) (*(volatile int16_t *)(uintptr_t)(a))
#define HW32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* TOM's GPU registers and local RAM (jaguar.inc). */
#define G_FLAGS 0xF02100u
#define G_END   0xF0210Cu
#define G_PC    0xF02110u
#define G_CTRL  0xF02114u
#define G_RAM   0xF03000u

/* Two truths that live in two places, checked where both are visible. */
_Static_assert(VIEW_BYTES == VIEW0_SIZE, "frame buffer size: csrc/road.h vs tools/memory_map.py");
_Static_assert(VIEW_ROWS == VIEW_H, "viewport rows: csrc/road.h vs tools/memory_map.py");
_Static_assert(CAR_FRAME_BYTES == CAR_BYTES, "ship frame size: csrc/road.h vs tools/memory_map.py");
_Static_assert(ROAD_CMD_MAX * 4 <= CMD_BUF_SIZE, "command list does not fit CMD_BUF");

/* Ceiling of the wait for DONE, in polls.  A frame's road is a few
 * milliseconds; this is about a second.  The wait must never be without an
 * exit: a stuck GPU becomes a number (GPU_FAIL), not a hang. */
#define GPU_GUARD 400000u

/* JagLBA's Gpu_Run (engine_c/jag_hw.h), verified on hardware:
 *  - DONE cleared and READ BACK: the 68000's writes are posted, and the read
 *    is what pushes them (and the command list before them) out before the
 *    GPU starts reading;
 *  - stop, and make sure: a G_PC written while the GPU still runs is lost;
 *  - the GPU stops itself at the end (src/gpu_road.s). */
static int gpu_run(uint32_t entry)
{
    HW32(GPU_DONE) = 0;
    (void)HW32(GPU_DONE);
    HW32(G_CTRL) = 0;
    (void)HW32(G_CTRL);
    HW32(G_FLAGS) = 0;
    HW32(G_END) = 0x00070007u;
    HW32(G_PC) = entry;
    (void)HW32(G_PC);
    HW32(G_CTRL) = 1;
    for (uint32_t i = 0; i < GPU_GUARD; i++)
        if (HW32(GPU_DONE))
            return 1;
    return 0;
}

/* Ticks the demo holds its last frame before starting again. */
#define DEMO_PAUSE 72

void c_render(void);

static uint16_t grid[SR_GRID_ROWS * 7];
static sr_road road;
static sr_play play;
static int result;
static uint32_t pause;

static void demo_start(void)
{
    const uint8_t *demo = (const uint8_t *)(uintptr_t)ROM_DEMO;
    road_load((const uint8_t *)(uintptr_t)ROM_ROADS, DEMO_ROAD, &road, grid);
    sr_play_init(&play, &road, DEMO_DATA(demo), DEMO_LEN(demo));
    result = SR_RES_RUNNING;
    pause = 0;
    HW32(DEMO_TICK) = 0;
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
    c_render();                 /* frame 1 ready before the OP list exists */
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

/* One frame, after the ticks of this video frame.  Frame k is drawn into
 * VIEW(k&1) while RMAC shows k-1 from the other; ROAD_SEQ = k hands it over,
 * and RMAC points the OP at it at the next video interrupt. */
static int gpu_launch(uint32_t list)
{
    HW32(GPU_CMD) = list;
    if (gpu_run(G_RAM))
        return 1;
    HW32(GPU_FAIL) = HW32(GPU_FAIL) + 1;
    return 0;
}

void c_render(void)
{
    uint32_t k = HW32(ROAD_SEQ) + 1;
    road_ctx c;
    c.trekg = (const uint8_t *)(uintptr_t)TREKG_BUF;
    c.trekg_at = TREKG_BUF;
    c.tables = (const uint8_t *)(uintptr_t)ROM_TABLES;
    c.view = (k & 1) ? VIEW1 : VIEW0;
    c.backdrop = BACKDROP_BUF;
    c.cmd = (uint32_t *)(uintptr_t)CMD_BUF;
    uint8_t *view = (uint8_t *)(uintptr_t)c.view;

    road_compose(&c, &play);
    ship_pose_t s = ship_pose(&play, play.tick, play.on_sticky);

    /* 1. backdrop + records before the ship (GPU); 2. ship and shadow
     * (68000); 3. records after the ship (GPU).  The GPU's epilogue pushes
     * its pixels out before DONE, so the 68000 reads finished pixels. */
    gpu_launch(CMD_BUF);
    road_ship(&c, view, (const uint8_t *)(uintptr_t)CARS_BUF, s);
    road_shadow(&c, view, &play, s);
    gpu_launch(CMD_BUF + 4u * (uint32_t)c.second);

    HW32(ROAD_SEQ) = k;
}
