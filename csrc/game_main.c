/* JagSkyroads - the game on the 68000.
 *
 * c_tick runs once per 36.0036 Hz game tick (RMAC counts them in the video
 * interrupt), c_render once per video frame that had ticks.
 *
 * THE FLOW is SKYROADS.EXE's main (0x1b8), not skyroads-port's game.c:
 *   intro (csrc/front.c) -> if it ran to the end, the attract demo, then the
 *   intro again; a key during it -> the main menu -> the road map -> a road.
 *   A road (play_road, fn_2b21): its first frame, a fade-in with the game
 *   FROZEN, the game; at the end "Road Completed" (or "The End") for 27
 *   ticks, a fade-out.  Then: completed -> the road map, the next road
 *   selected; ESC (OPTION) -> the road map; lost -> the same road at once.
 *   The demo: ESC -> main menu, its end -> the intro.  The front end BLOCKS
 *   inside c_tick (it waits ticks itself, front.c fe_tick), as the original
 *   blocks in its menus.
 *
 * ATTRACT: DEMO.REC drives road 0 through skyroads-port's play.c, and every
 *   tick is compared with the PC oracle's record (csrc/shared.h, ROM_DEMOX);
 *   the first tick that differs goes to DEMO_BAD, which RMAC shows in the
 *   margins.
 * PLAY: the pad drives play.c (A/B/C jump, PAUSE pauses, OPTION is ESC).  On
 *   the KEYPAD, never on the game controls: * and # step to the previous /
 *   next road (a test instrument), 0 shows the frame's timings, 9 checks the
 *   GPU's composers against road_compose (below).
 *
 * SOUND (M4): the songs and the effects are Jerry's (src/dsp_audio.s, an
 *   OPL2 synthesised on the DSP); this side only says which, through the
 *   MUS_* and SND_* cells - see audio_post.
 *
 * The boundary with RMAC is memory cells: PAD_STATE/PAD_KEYS in, GAME_MODE,
 * ROAD_SEQ and the demo check out.
 */
#include <string.h>
#include "shared.h"
#include "road.h"
#include "hud.h"
#include "mem_map.h"
#include "front.h"

#define HW16(a) (*(volatile int16_t *)(uintptr_t)(a))
#define HW32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* TOM's GPU registers and local RAM, and the CLUT (jaguar.inc). */
#define G_FLAGS  0xF02100u
#define G_END    0xF0210Cu
#define G_PC     0xF02110u
#define G_CTRL   0xF02114u
#define G_RAM    0xF03000u
#define CLUT_REG 0xF00400u

/* Two truths that live in two places, checked where both are visible. */
_Static_assert(VIEW_BYTES == VIEW0_SIZE && VIEW_BYTES == VIEW1_SIZE && VIEW_BYTES == VIEW2_SIZE,
               "frame buffer size: csrc/road.h vs tools/memory_map.py");
_Static_assert(VIEW_ROWS == GAME_H, "screen rows: csrc/road.h vs tools/memory_map.py");
_Static_assert(CAR_FRAME_BYTES == CAR_BYTES, "ship frame size: csrc/road.h vs tools/memory_map.py");
_Static_assert(ROAD_CMD_MAX * 4 <= CMD_BUF_SIZE, "command list does not fit CMD_BUF");
_Static_assert(ROAD_CMD_MAX * 4 <= CMD_CHK_SIZE, "command list does not fit CMD_CHK");
_Static_assert(COMP_POS == COMP_IN + CIN_POS && COMP_BASE == COMP_IN + CIN_BASE
               && COMP_QUAD0 == COMP_IN + CIN_QUAD0 && COMP_QUAD1 == COMP_IN + CIN_QUAD1
               && COMP_WIN == COMP_IN + CIN_WIN && COMP_ROWS == CIN_ROWS
               && CIN_BYTES <= COMP_IN_SIZE,
               "GPU composers' input: csrc/road.h CIN_* vs tools/memory_map.py");
_Static_assert(SHIP_SRC == SHIP_IN + SIN_SRC && SHIP_ROW0 == SHIP_IN + SIN_ROW0
               && SHIP_LEFT == SHIP_IN + SIN_LEFT && SHIP_MASK == SHIP_IN + SIN_MASK
               && SHIP_SROW0 == SHIP_IN + SIN_SROW0 && SHIP_SLEFT == SHIP_IN + SIN_SLEFT
               && SHIP_CLIP == SHIP_IN + SIN_CLIP && SHIP_SCLIP == SHIP_IN + SIN_SCLIP
               && SIN_BYTES <= SHIP_IN_SIZE && SHADOW_MASK_BYTES <= SHADOW_MASKS_SIZE,
               "the GPU's ship input: csrc/road.h SIN_* vs tools/memory_map.py");

/* Ceiling of the wait for DONE, in polls (about a second with the pause
 * below).  The wait must never be without an exit: a stuck GPU becomes a
 * number (GPU_FAIL). */
#define GPU_GUARD 60000u

/* A pause that stays off the bus.  The 68000 polling DRAM in a tight loop
 * slows the GPU's DRAM loads from 4.8 to 13.8 clk each (JaguarDocs hw/01
 * s.6.1, T-MEM-13, hardware); three MULUs by $FFFF are ~210 cycles of ALU
 * with one instruction fetch each, so a poll touches the bus a few times
 * every ~16 us instead of every few cycles. */
static inline void bus_pause(void)
{
    uint32_t a = 0, b = 0xFFFFu;
    __asm__ volatile("mulu.w %1,%0\n\tmulu.w %1,%0\n\tmulu.w %1,%0"
                     : "+d"(a) : "d"(b));
}

/* JagLBA's Gpu_Run (engine_c/jag_hw.h), verified on hardware:
 *  - DONE cleared and READ BACK: the 68000's writes are posted, and the read
 *    is what pushes them (and the command list before them) out before the
 *    GPU starts reading;
 *  - stop, and make sure: a G_PC written while the GPU still runs is lost;
 *  - the GPU stops itself at the end (src/gpu_road.s). */
static int gpu_run(uint32_t entry_pc)
{
    HW32(GPU_DONE) = 0;
    (void)HW32(GPU_DONE);
    HW32(G_CTRL) = 0;
    (void)HW32(G_CTRL);
    HW32(G_FLAGS) = 0;
    HW32(G_END) = 0x00070007u;
    HW32(G_PC) = entry_pc;
    (void)HW32(G_PC);
    HW32(G_CTRL) = 1;
    for (uint32_t i = 0; i < GPU_GUARD; i++) {
        if (HW32(GPU_DONE))
            return 1;
        bus_pause();
    }
    return 0;
}

#define PADB(n)       (1u << (n))
#define PAD_JUMP      (PADB(PAD_B_A) | PADB(PAD_B_B) | PADB(PAD_B_C))

void c_render(void);

/* ---- SOUND: what Jerry plays (src/dsp_audio.s) --------------------------
 * The cells: MUS_ADR, MUS_LOOP then MUS_SEQ + 1 start a stream of
 * cart/muzax.bin (tools/muzax.py: 0 INIT, 1 + n song n, 15 silence);
 * SND_ADR, SND_LEN then SND_SEQ + 1 an effect of cart/sfx.bin.  SEQ LAST,
 * and ONE delivery per video frame (Keen Dreams' protocol, verified on
 * hardware): the DSP reads the cells at its own 180 Hz, and two deliveries
 * close together could hand it one effect's address with another's length.
 * Requests are therefore only noted here and posted by audio_post, at most
 * once per frame; the last effect asked wins, as on the Sound Blaster
 * (play_sfx 0x3c2 stops the current one).
 *
 * The songs as the original picks them: the menu's (1) under the attract
 * demo (its menu runs the demo); a game started from it gets 2 + rand % 12,
 * never the same twice in a row (main 0x29f); deaths, completed roads and
 * the keypad's road steps keep it.  A song already playing is not restarted
 * (music_start 0x57b1). */
static int song_now = -1, song_want = -1, game_song = -1, sfx_want = -1;
static uint32_t aud_vi, rnd;
#define SONG_SILENCE 14                 /* muzax.bin's stream 15 */

/* music_start 0x57a8: the song already playing goes on; otherwise the
 * music stops, and starts again only if the sound is on (ds:0x4528). */
void music(int n)
{
    if (n == song_now)
        return;
    song_now = -1;
    song_want = SONG_SILENCE;
    if (cfg.sound_off)
        return;
    song_now = n;
    song_want = n;
}

void music_stop(void)
{
    song_now = -1;
    song_want = SONG_SILENCE;
}

void sfx(int n)                         /* play_sfx 0x3c2: gated by sound off */
{
    if (!cfg.sound_off)
        sfx_want = n;
}

void sfx_voice(void)                    /* INTRO.SND, 0x4775: not gated */
{
    sfx_want = 5;
}

void audio_post(void)
{
    uint32_t vi = HW32(IRQ_TICK);
    if (vi == aud_vi)
        return;
    aud_vi = vi;
    if (song_want >= 0) {
        const uint8_t *mz = (const uint8_t *)(uintptr_t)ROM_MUZAX;
        uint32_t k = 1u + (uint32_t)song_want;
        HW32(MUS_ADR) = ROM_MUZAX + get32(mz + 4 + 8 * k);
        HW32(MUS_LOOP) = ROM_MUZAX + get32(mz + 8 + 8 * k);
        HW32(MUS_SEQ) = HW32(MUS_SEQ) + 1;
        song_want = -1;
    }
    if (sfx_want >= 0) {
        const uint8_t *sx = (const uint8_t *)(uintptr_t)ROM_SFX;
        uint32_t k = (uint32_t)sfx_want;
        HW32(SND_ADR) = ROM_SFX + get32(sx + 4 + 8 * k);
        HW32(SND_LEN) = get32(sx + 8 + 8 * k);
        HW32(SND_SEQ) = HW32(SND_SEQ) + 1;
        sfx_want = -1;
    }
}

/* ---- PERF: an instrument on the KEYPAD (key 0), off by default ----------
 * It answers one question - where a frame's time goes - and is switched off
 * when it has answered.  Times are in video HALF-LINES (31.8 us NTSC), the
 * unit VC counts in.  On screen, top left, after the frame is complete:
 *   ticks  compose  gpu1  ship+shadow  gpu2  gauges  |  frames drawn last second */
#define VC_REG 0xF00006u
static int perf_on;
static uint8_t pf_fg, pf_bg;      /* brightest / darkest entry of the CLUT */
static uint32_t pf_tick, pf_fps_count, pf_fps, pf_fps_mark;
static uint32_t pf_sum[6], pf_shown[7];   /* per-second sums, latched averages */
static int ovl_dirty;             /* the overlay buffer needs redrawing */

/* ---- LOG: one line a second to COM8 through the GameDrive (src/gd.s) ---
 * The on-screen numbers cannot see what lies OUTSIDE their phases, nor what
 * they cost themselves; this can.  Sums over the second, in half-lines:
 *   T ticks  I input+pose  G gpu1  N ship+shadow  H gpu2  D gauges
 *   O the overlay (draw + blit)  W wait for a free buffer  L the last line's
 *   own sending  A the whole second, by the clock  -> A - all = the unseen.
 * Read on the PC with tools/comlog.py.  Hex, so no division on the 68000. */
enum { LG_TICK, LG_IN, LG_G1, LG_SHIP, LG_G2, LG_HUD, LG_OVL, LG_WAIT, LG_SEND, LG_N };
static uint32_t lg_sum[LG_N], lg_frames, lg_ticks, lg_mark, lg_t0, lg_drop0;
static int lg_started;
static char lg_line[256];

/* ---- CHECK: an instrument on the KEYPAD (key 9), off by default ---------
 * One question: does the GPU compose, on the real machine, the list
 * road_compose composes?  (The PC answers it for the logic, running the GPU
 * binary in an interpreter: tools/oracle/gpu_check.py; this answers it for
 * the hardware.)  While on, every frame is composed a second time by the
 * 68000 into CMD_CHK and compared, both lists and the second launch's
 * address; on screen, third row: frames checked, frames that differed. */
static int check_on;
static uint32_t chk_frames, chk_bad;

/* Never backwards: VC can already be past VI while the interrupt that
 * advances IRQ_TICK has not run yet (masked, or the bus held), and the
 * reading then falls one frame short; the first COM8 log showed it as
 * phases of ~65000 half-lines.  Such a reading becomes the last one. */
static uint32_t clock_last;

static uint32_t clock_hl(void)
{
    uint32_t hl = HW16(VID_NTSC) ? 524u : 624u;
    for (;;) {
        uint32_t t = HW32(IRQ_TICK);
        uint32_t vc = (uint32_t)(HW16(VC_REG) & 0x7FF);
        if (HW32(IRQ_TICK) != t)
            continue;
        /* IRQ_TICK advances at VI, not at VC = 0: count from VI */
        uint32_t vi = (uint32_t)(uint16_t)HW16(VI_VAL);
        uint32_t ph = vc >= vi ? vc - vi : vc + hl - vi;
        uint32_t now = t * hl + ph;
        if ((int32_t)(now - clock_last) < 0)
            now = clock_last;
        clock_last = now;
        return now;
    }
}

static uint16_t grid[SR_GRID_ROWS * 7];
static sr_road road;
static sr_play play;
static int result;
static int demo;              /* 1: attract demo */
static int paused;
static uint16_t pad_prev, keys_prev;
/* THREE frame buffers: the one on screen, the one the next video interrupt
 * will show (SetView only updates OBJ_SHADOW, which the interrupt copies:
 * a swap takes effect one interrupt late), and the one being drawn.  With
 * two, the frame being drawn was often the one on screen: the road flickered
 * (session 2, hardware). */
#define N_VIEWS 3
static const uint32_t views[N_VIEWS] = { VIEW0, VIEW1, VIEW2 };
static int view_next;          /* the buffer the next frame goes into */
static hud_state hud_st[N_VIEWS];   /* what each frame buffer's dashboard shows */

/* ---- THE FLOW (main 0x1b8, play_road fn_2b21) ------------------------- */
enum { AT_INTRO, AT_MENU, AT_GOMENU, AT_REPLAY, AT_STEP };
static int live;                /* a road is running: ticks and frames go to it */
static int at = AT_INTRO;       /* where the director goes next */
static int menu_first;          /* main's -8(bp): the menu follows the intro */
static int last_road;           /* "The End" if this one completes (0x339) */
static int hud_prime;           /* the next frame primes all dashboards */
static const uint8_t *game_pal; /* the road's palette, 6-bit (ds:0x41d0) */
#define R_STEP 100              /* keypad * / #: another road, at once */

/* A road from scratch: grid, pristine screen (its world + the dashboard),
 * palette.  The original loads all three at road start (game.c start_road,
 * render.c sr_render_set_world).  It runs at the bottom of a fade, the
 * screen black: road_begin fades the palette in. */
static void road_start(int e, int is_demo)
{
    const uint8_t *demo_blob = (const uint8_t *)(uintptr_t)ROM_DEMO;
    int world = e == 0 ? 0 : (e - 1) / 3;
    road_load((const uint8_t *)(uintptr_t)ROM_ROADS, e, &road, grid);
    hud_pristine((uint8_t *)(uintptr_t)PRISTINE,
                 (const uint8_t *)(uintptr_t)(ROM_WORLDS + (uint32_t)world * WORLD_STRIDE),
                 (const uint8_t *)(uintptr_t)ROM_DASH);
    game_pal = (const uint8_t *)(uintptr_t)(ROM_PAL + (uint32_t)e * PAL_BYTES);
    int lmax = -1, lmin = 1 << 30;
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = game_pal + 3 * i;
        int l = 2 * c[0] + 3 * c[1] + c[2];
        if (l > lmax) { lmax = l; pf_fg = (uint8_t)i; }
        if (l < lmin) { lmin = l; pf_bg = (uint8_t)i; }
    }
    if (is_demo)
        sr_play_init(&play, &road, DEMO_DATA(demo_blob), DEMO_LEN(demo_blob));
    else
        sr_play_init(&play, &road, NULL, 0);
    demo = is_demo;
    for (int i = 0; i < N_VIEWS; i++)
        hud_st[i].valid = 0;
    result = SR_RES_RUNNING;
    paused = 0;
    HW16(GAME_MODE) = (int16_t)(is_demo ? 0 : 1);
    if (is_demo)
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
    road_gpu_quads((uint8_t *)(uintptr_t)COMP_IN, (const uint8_t *)(uintptr_t)ROM_TABLES);
    road_shadow_masks((uint8_t *)(uintptr_t)SHADOW_MASKS, (const uint8_t *)(uintptr_t)ROM_TABLES);
    /* The interrupts are still off: nothing here may wait.  The front end
     * starts at the first tick (director). */
    if (!fe_init())
        return 0;               /* front.bin is not this code's: Panic */
    fe_cfg_load();              /* main 0x202 */
    HW16(GAME_MODE) = 2;        /* black margins outside the demo */
    music(0);                   /* main 0x207 */
    return C_MAGIC;
}

/* The attract demo's tick, checked against the oracle. */
static void demo_tick(void)
{
    const uint8_t *x = (const uint8_t *)(uintptr_t)ROM_DEMOX;
    uint32_t n_exp = get32(x);
    uint32_t t = HW32(DEMO_TICK);

    sr_input none;
    memset(&none, 0, sizeof none);
    sr_play_input(&play, &none);
    result = sr_play_tick(&play);
    if (play.pending_sfx) {             /* fn_03c2's calls */
        sfx(play.pending_sfx - 1);
        play.pending_sfx = 0;
    }
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
        mismatch(t);
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

void c_render(void);

/* A road (fn_2b21 up to the game loop): built while the screen is black,
 * its first frame drawn and shown, then the fade-in - with the game
 * FROZEN, as fn_4b72 blocks there (0x2c35) before fn_1f2c starts. */
static void road_begin(int e, int is_demo)
{
    if (!is_demo) {                     /* 0x30e..0x361 */
        int n = 0;
        for (int i = 0; i < 30; i++)
            n += cfg.done[i] != 0;
        last_road = cfg.done[fe_road] == 0 && n == 29;
    }
    road_start(e, is_demo);
    hud_prime = 1;
    live = 1;
    c_render();
    fe_wait_shown();
    fe_fade_full(game_pal, 1, 36);
    fe_flush();
    pad_prev = (uint16_t)HW16(PAD_STATE);
    keys_prev = (uint16_t)HW16(PAD_KEYS);
}

/* The end of a road (fn_2b21 after the loop, then main 0x36a..0x3b7): the
 * text over the last frame and 27 ticks if completed, the fade-out, and
 * where to go next. */
static void road_end(int r)
{
    fe_wait_shown();
    if (r == SR_RES_COMPLETE) {
        fe_text((uint8_t *)(uintptr_t)HW32(ROAD_VIEW), last_road ? 0x84 : 0x68, 0x50,
                last_road ? "The End" : "Road Completed", 0x63);
        fe_wait(27);
    }
    fe_fade_full(game_pal, 0, 36);
    live = 0;
    paused = 0;
    if (demo) {
        at = r == SR_RES_QUIT ? AT_MENU : AT_INTRO;
    } else if (r == R_STEP) {
        at = AT_STEP;
    } else if (r == SR_RES_COMPLETE) {
        cfg.done[fe_road]++;
        fe_road++;
        fe_cfg_save();
        at = AT_GOMENU;
    } else if (r == SR_RES_QUIT) {
        at = AT_GOMENU;
    } else {
        at = AT_REPLAY;
    }
}

/* main's loop between roads: blocks in the front end until a road is
 * chosen, then starts it. */
static void director(void)
{
    if (at <= AT_GOMENU) {
        HW16(GAME_MODE) = 2;
        fe_show();
    }
    for (;;) {
        if (at == AT_INTRO) {           /* 0x219 */
            menu_first = 1;
            if (!fe_intro()) {          /* ran to the end: the demo (0x229) */
                menu_first = 0;
                road_begin(0, 1);
                return;
            }
            at = AT_MENU;
        }
        if (at == AT_MENU) {            /* 0x232 */
            fe_mainmenu(!menu_first);
            menu_first = 0;
            at = AT_GOMENU;
        }
        if (at == AT_GOMENU) {          /* 0x282 */
            if (fe_gomenu()) {
                at = AT_MENU;
                continue;
            }
            /* a song for the road: 2 + rand % 12, not the last one (0x29c) */
            rnd = rnd * 1103515245u + 12345u + HW32(IRQ_TICK);
            int k = (int)((rnd >> 16) % 12u);
            if (k == game_song)
                k = (k + 1) % 12;
            game_song = k;
            music(2 + k);
        }
        break;
    }
    road_begin(fe_road + 1, 0);         /* AT_GOMENU, AT_REPLAY, AT_STEP */
}

static void c_tick_inner(void);

void c_tick(void)
{
    uint32_t t0 = clock_hl();
    if (HW16(PAD_KEYS) & ~keys_prev & PADB(KEY_B_0))
        perf_on = !perf_on;
    if (HW16(PAD_KEYS) & ~keys_prev & PADB(KEY_B_9)) {
        check_on = !check_on;
        chk_frames = chk_bad = 0;
    }
    if (HW16(PAD_KEYS) & ~keys_prev & (PADB(KEY_B_0) | PADB(KEY_B_9)))
        ovl_dirty = 1;
    c_tick_inner();
    audio_post();
    uint32_t dt = clock_hl() - t0;
    pf_tick += dt;
    lg_sum[LG_TICK] += dt;
    lg_ticks++;
}

static void c_tick_inner(void)
{
    if (!live) {
        director();
        return;
    }
    uint16_t pad = (uint16_t)HW16(PAD_STATE), keys = (uint16_t)HW16(PAD_KEYS);
    uint16_t edge = pad & (uint16_t)~pad_prev, kedge = keys & (uint16_t)~keys_prev;
    pad_prev = pad;
    keys_prev = keys;

    /* the demo: ESC only (0x21e9; its input is DEMO.REC's) */
    if (demo) {
        if (edge & PADB(PAD_B_OPT)) {
            road_end(SR_RES_QUIT);
            return;
        }
        demo_tick();
        if (result != SR_RES_RUNNING)
            road_end(result);
        return;
    }

    if (kedge & (PADB(KEY_B_STAR) | PADB(KEY_B_HASH))) {
        fe_road = (fe_road + ((kedge & PADB(KEY_B_STAR)) ? 29 : 1)) % 30;
        road_end(R_STEP);
        return;
    }
    /* P pauses; while paused ESC quits, any other key goes on (0x2050) */
    if (paused) {
        if (edge & PADB(PAD_B_OPT))
            road_end(SR_RES_QUIT);
        else if (edge)
            paused = 0;
        return;
    }
    if (edge & PADB(PAD_B_PAUSE)) {
        paused = 1;
        return;
    }
    if (edge & PADB(PAD_B_OPT)) {       /* ESC: result 7 (0x21fa) */
        road_end(SR_RES_QUIT);
        return;
    }

    /* fn_074c's keyboard path, on the pad: steer, throttle, jump. */
    sr_input in;
    memset(&in, 0, sizeof in);
    in.held[SR_KEY_UP] = (pad & PADB(PAD_B_UP)) != 0;
    in.held[SR_KEY_DOWN] = (pad & PADB(PAD_B_DOWN)) != 0;
    in.held[SR_KEY_LEFT] = (pad & PADB(PAD_B_LEFT)) != 0;
    in.held[SR_KEY_RIGHT] = (pad & PADB(PAD_B_RIGHT)) != 0;
    in.held[SR_KEY_JUMP] = (pad & PAD_JUMP) != 0;
    sr_play_input(&play, &in);
    result = sr_play_tick(&play);
    if (play.pending_sfx) {
        sfx(play.pending_sfx - 1);
        play.pending_sfx = 0;
    }
    if (result != SR_RES_RUNNING)
        road_end(result);
}

/* ---- the Blitter: the pristine viewport into a frame buffer ------------
 * One blit, phrase mode (XADDPHR: opaque copies, which need no DCOMPEN),
 * rows of 320 with the step back to x = 0 at the end of each: Keen Dreams'
 * BLIT_STEP_ROW and B_CMD_OPAQUE, verified on hardware.  The GPU copied
 * long by long between two DRAM pages and paid ~20 ms; a large blit is
 * nearly free (Keen Dreams measured 76.8 KB in 4.3 ms).
 * B_COUNT and the PIXEL pointers are CONSUMED by every blit: written every
 * time, next to B_CMD. */
#define BLIT_FLAGS (JAG_PITCH1 | JAG_PIXEL8 | JAG_WID320 | JAG_XADDPHR)
#define BLIT_CMD   (JAG_SRCEN | JAG_UPDA1 | JAG_UPDA2 | JAG_LFU_REPLACE)
#define BLIT_STEP  ((1u << 16) | (uint16_t)(-320))
#define BLIT_GUARD 200000u

static void blit_wait(void)
{
    for (uint32_t i = 0; i < BLIT_GUARD; i++)
        if (HW32(JAG_B_CMD) & 1)
            return;
    HW32(BLIT_STALL) = HW32(BLIT_STALL) + 1;
}

static void blit_rows(uint32_t dst, uint32_t src, uint32_t rows);

static void blit_view(uint32_t dst, uint32_t src)
{
    blit_rows(dst, src, VIEW_COPY_ROWS);
}

/* rows x 320 bytes, src -> dst, one blit (both 320 wide). */
static void blit_rows(uint32_t dst, uint32_t src, uint32_t rows)
{
    blit_wait();
    HW32(JAG_A1_BASE) = dst;
    HW32(JAG_A1_FLAGS) = BLIT_FLAGS;
    HW32(JAG_A1_CLIP) = 0;
    HW32(JAG_A1_STEP) = BLIT_STEP;
    HW32(JAG_A2_BASE) = src;
    HW32(JAG_A2_FLAGS) = BLIT_FLAGS;
    HW32(JAG_A2_STEP) = BLIT_STEP;
    HW32(JAG_A1_PIXEL) = 0;
    HW32(JAG_A2_PIXEL) = 0;
    HW32(JAG_B_COUNT) = (rows << 16) | 320u;
    HW32(JAG_B_CMD) = BLIT_CMD;
}

/* One frame, after the ticks of this video frame.  Frame k is drawn into
 * views[view_next] while the OP shows k-1 or k-2 from the others; ROAD_VIEW
 * and ROAD_SEQ = k hand it over, RMAC's SetView points OBJ_SHADOW at it, and
 * the video interrupt after that puts it on screen. */
static void gpu_launch(void)
{
    if (!gpu_run(G_RAM))
        HW32(GPU_FAIL) = HW32(GPU_FAIL) + 1;
}

/* The instruments' numbers live in OVL_BUF (320 wide, like a frame): drawn
 * there once a second, when they are latched, and copied into every frame
 * by the Blitter - the frame's top rows are restored from the pristine
 * screen every time, and redrawing the digits there each frame cost more
 * than what they measured (M3, hardware).  Rows: timings (two) and the
 * CHECK's counters (one), 240 columns. */
#define OVL_W      240
#define OVL_ROWS   76
#define OVL_STEP   ((1u << 16) | (uint16_t)(-OVL_W))

static void ovl_draw(void)
{
    const uint8_t *hud = (const uint8_t *)(uintptr_t)ROM_HUD;
    uint8_t *o = (uint8_t *)(uintptr_t)OVL_BUF;
    uint32_t bg4 = pf_bg * 0x01010101u;
    for (int y = 0; y < OVL_ROWS; y++) {
        uint32_t *q = (uint32_t *)(void *)(o + (int32_t)(int16_t)y * 320);
        for (int i = 0; i < OVL_W / 4; i++)
            q[i] = bg4;
    }
    if (perf_on)
        /* row 1: ticks, compose, gpu1, ship+shadow; row 2: gpu2, gauges, fps */
        for (int i = 0; i < 7; i++)
            hud_number(o, hud, 6 + 60 * (i % 4), 6 + 24 * (i / 4), pf_shown[i], 3, 3, pf_fg, pf_bg);
    if (check_on) {
        hud_number(o, hud, 6, 54, chk_frames, 5, 3, pf_fg, pf_bg);
        hud_number(o, hud, 126, 54, chk_bad, 5, 3, pf_fg, pf_bg);
    }
}

static void ovl_blit(uint32_t view)
{
    blit_wait();
    HW32(JAG_A1_BASE) = view;
    HW32(JAG_A1_FLAGS) = BLIT_FLAGS;
    HW32(JAG_A1_CLIP) = 0;
    HW32(JAG_A1_STEP) = OVL_STEP;
    HW32(JAG_A2_BASE) = OVL_BUF;
    HW32(JAG_A2_FLAGS) = BLIT_FLAGS;
    HW32(JAG_A2_STEP) = OVL_STEP;
    HW32(JAG_A1_PIXEL) = 0;
    HW32(JAG_A2_PIXEL) = 0;
    HW32(JAG_B_COUNT) = ((uint32_t)OVL_ROWS << 16) | OVL_W;
    HW32(JAG_B_CMD) = BLIT_CMD;
    blit_wait();
}

/* The CHECK instrument (above): road_compose into CMD_CHK, compared with
 * the GPU's CMD_BUF up to each list's end. */
static void check_lists(road_ctx *c)
{
    const uint32_t *gpu = (const uint32_t *)(uintptr_t)CMD_BUF;
    c->cmd = (uint32_t *)(uintptr_t)CMD_CHK;
    road_compose(c, &play);
    int bad = HW32(GPU_CMD) != CMD_BUF + 4u * (uint32_t)c->second;
    int ends = 0;
    for (int i = 0; ends < 2 && i < ROAD_CMD_MAX && !bad; i += 2) {
        if (gpu[i] != c->cmd[i] || gpu[i + 1] != c->cmd[i + 1])
            bad = 1;
        if (c->cmd[i] == ROAD_OP_END)
            ends++;
    }
    chk_frames++;
    chk_bad += (uint32_t)bad;
}

/* Frame k goes into the buffer frame k-3 used.  The video interrupt puts a
 * finished frame on screen at once (ViHandler), so that buffer is free as
 * soon as the screen shows k-2 or later; it waits only when a frame took
 * less than a VI.  At $2700 for the check, then STOP: an interrupt between
 * the two cannot be slept through. */
static void wait_shown(uint32_t need)
{
    for (;;) {
        __asm__ volatile("move.w #0x2700,%%sr" ::: "memory");
        if (HW32(ROAD_SHOWN) >= need)
            break;
        __asm__ volatile("stop #0x2000" ::: "memory");
    }
    __asm__ volatile("move.w #0x2000,%%sr" ::: "memory");
}

static char *lg_hex(char *o, char key, uint32_t v, int digits)
{
    *o++ = ' ';
    *o++ = key;
    *o++ = '=';
    for (int i = digits - 1; i >= 0; i--)
        *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return o;
}

/* After a frame: once a second, the line.  Its own cost goes into the NEXT
 * second's L, so A still adds up. */
static void lg_frame(void)
{
    uint32_t now = clock_hl();
    if (!lg_started) {
        lg_started = 1;
        lg_t0 = now;
        lg_mark = HW32(IRQ_TICK);
        lg_drop0 = HW32(TICK_DROP);
        for (int i = 0; i < LG_N; i++)
            lg_sum[i] = 0;
        lg_frames = lg_ticks = 0;
        return;
    }
    lg_frames++;
    if (HW32(IRQ_TICK) - lg_mark < (uint32_t)(uint16_t)HW16(TICK_HZ))
        return;
    uint32_t drop = HW32(TICK_DROP);
    char *o = lg_line;
    *o++ = 'S';
    o = lg_hex(o, 'd', HW32(DEMO_TICK), 4);
    o = lg_hex(o, 'r', (uint32_t)(uint16_t)HW16(CUR_ROAD), 2);
    o = lg_hex(o, 'm', (uint32_t)(uint16_t)HW16(GAME_MODE), 1);
    o = lg_hex(o, 'f', lg_frames, 2);
    o = lg_hex(o, 't', lg_ticks, 2);
    o = lg_hex(o, 'x', drop - lg_drop0, 2);
    o = lg_hex(o, 'A', now - lg_t0, 4);
    static const char keys[LG_N] = { 'T', 'I', 'G', 'N', 'H', 'D', 'O', 'W', 'L' };
    for (int i = 0; i < LG_N; i++)
        o = lg_hex(o, keys[i], lg_sum[i], 4);
    o = lg_hex(o, 'F', HW32(GPU_FAIL), 2);
    o = lg_hex(o, 'B', HW32(BLIT_STALL), 2);
    o = lg_hex(o, 'P', HW32(GD_DROP), 2);
    o = lg_hex(o, 'K', (uint32_t)perf_on | (uint32_t)check_on << 1, 1);
    /* Jerry: samples produced, I2S interrupts, idle-loop turns with the ring
     * full (the time left over), interrupts that found it empty (cumulative) */
    {
        static uint32_t done0, irq0, spin0;
        uint32_t done = HW32(SND_DONE), irq = HW32(SND_IRQ), spin = HW32(SND_SPIN);
        o = lg_hex(o, 'a', done - done0, 5);
        o = lg_hex(o, 'q', irq - irq0, 5);
        o = lg_hex(o, 'p', spin - spin0, 6);
        o = lg_hex(o, 's', HW32(SND_STARVE), 4);
        done0 = done;
        irq0 = irq;
        spin0 = spin;
    }
    *o = 0;
    for (int i = 0; i < LG_N; i++)
        lg_sum[i] = 0;
    lg_frames = lg_ticks = 0;
    lg_drop0 = drop;
    lg_mark = HW32(IRQ_TICK);
    lg_t0 = now;
    uint32_t fn = HW32(GD_SEND_FN);
    if (fn) {
        ((void (*)(const char *))(uintptr_t)fn)(lg_line);
        lg_sum[LG_SEND] = clock_hl() - now;
    }
}

void c_render(void)
{
    if (!live)
        return;                         /* between roads: the front end's */
    uint32_t k = HW32(ROAD_SEQ) + 1;
    _Static_assert(N_VIEWS == 3, "wait_shown assumes three frame buffers");
    /* Frames 1 and 2 have buffers no frame used before; frame 1 is drawn
     * by c_init with the interrupts still off, where a STOP never wakes
     * (k - 2 unsigned: the first try hung the boot on COL_BOOT). */
    uint32_t tw = clock_hl();
    if (k > 2)
        wait_shown(k - 2);
    road_ctx c;
    c.trekg = (const uint8_t *)(uintptr_t)TREKG_BUF;
    c.trekg_at = TREKG_BUF;
    c.tables = (const uint8_t *)(uintptr_t)ROM_TABLES;
    int vb = view_next;
    c.view = views[vb];
    c.pristine = PRISTINE;
    c.cmd = (uint32_t *)(uintptr_t)CMD_BUF;
    uint8_t *view = (uint8_t *)(uintptr_t)c.view;

    uint32_t t[6];
    t[0] = clock_hl();
    lg_sum[LG_WAIT] += t[0] - tw;
    /* The GPU's input first, on a free bus: during a blit the 68000 barely
     * gets a cycle (with the blit started first this phase measured ~120
     * half-lines, M3, hardware).  0. Then the Blitter restores the viewport
     * while the GPU composes; the GPU waits for it before the first span
     * (src/gpu_road.s). */
    road_gpu_input((uint8_t *)(uintptr_t)COMP_IN, &play, c.view);
    ship_pose_t s = ship_pose(&play, play.tick, play.on_sticky);
    road_ship_input((uint8_t *)(uintptr_t)SHIP_IN, &c, &play, s, CARS_BUF, SHADOW_MASKS);
    blit_view(c.view, PRISTINE);
    t[1] = clock_hl();

    /* ONE launch: the composers, the records before the ship, the ship and
     * its shadow (from SHIP_IN), the records after it - all on the GPU
     * (src/gpu_road.s).  Then the gauges, incrementally (68000).  The GPU's
     * epilogue pushes its pixels out before DONE.  The ship and second-list
     * phases of the log stay, now empty. */
    HW32(GPU_CMD) = 0;
    gpu_launch();
    t[2] = clock_hl();
    if (check_on)
        check_lists(&c);
    t[3] = clock_hl();
    t[4] = t[3];
    hud_update(view, (const uint8_t *)(uintptr_t)PRISTINE,
               (const uint8_t *)(uintptr_t)ROM_HUD, &play, &hud_st[vb]);
    t[5] = clock_hl();

    /* Numbers that change 36 times a second cannot be read off a television
     * (first try): each is the AVERAGE per frame over the last second,
     * latched once a second. */
    pf_fps_count++;
    pf_sum[0] += pf_tick;
    for (int i = 0; i < 5; i++) {
        pf_sum[i + 1] += t[i + 1] - t[i];
        lg_sum[LG_IN + i] += t[i + 1] - t[i];
    }
    if (HW32(IRQ_TICK) - pf_fps_mark >= (uint32_t)(uint16_t)HW16(TICK_HZ)) {
        pf_fps_mark = HW32(IRQ_TICK);
        pf_fps = pf_fps_count;
        for (int i = 0; i < 6; i++) {
            pf_shown[i] = pf_sum[i] / pf_fps_count;
            pf_sum[i] = 0;
        }
        pf_shown[6] = pf_fps;
        pf_fps_count = 0;
        ovl_dirty = 1;
    }
    pf_tick = 0;
    if (perf_on || check_on) {
        uint32_t to = clock_hl();
        if (ovl_dirty) {
            ovl_draw();
            ovl_dirty = 0;
        }
        ovl_blit(c.view);
        lg_sum[LG_OVL] += clock_hl() - to;
    }

    /* The first frame of a road, drawn while the screen is black: its
     * dashboard (redrawn from scratch: ~0.13 s on the 68000) goes into the
     * other two buffers by the Blitter, so the first frames of the fade-in
     * do not redraw it each (seen as a hitch at every road start). */
    if (hud_prime) {
        for (int i = 0; i < N_VIEWS; i++)
            if (i != vb) {
                blit_rows(views[i] + HUD_ROW0 * 320u, c.view + HUD_ROW0 * 320u,
                          VIEW_ROWS - HUD_ROW0);
                hud_st[i] = hud_st[vb];
            }
        blit_wait();
        hud_prime = 0;
    }

    HW32(ROAD_VIEW) = c.view;
    HW32(ROAD_SEQ) = k;
    view_next = vb == N_VIEWS - 1 ? 0 : vb + 1;
    lg_frame();
}
