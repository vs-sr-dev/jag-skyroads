/* JagSkyroads - the front end: intro, main menu, settings, help, road map.
 *
 * SKYROADS.EXE's own code, transcribed from the disassembly (objdump of the
 * load image; addresses below are image offsets), NOT skyroads-port's
 * game.c, which approximates these screens (no logo wipe, no captions, an
 * invented idle timeout, a brightened road name instead of a frame...).
 *
 * The original is VGA mode 13h: it draws straight into the visible memory,
 * sets the DAC, and BLOCKS while it waits, counting 36 Hz timer ticks in
 * ds:0x160c.  Here the same: FRONT_VIEW is the frame buffer the OP shows,
 * `dac` is the DAC (6 bits a gun) published to the CLUT through FADE_PAL,
 * and fe_tick() waits for and consumes one game tick, reads the pad and
 * feeds Jerry.  The front end runs inside c_tick and returns when a road is
 * chosen; src/main.s's loop accepts that GAME_DONE moved meanwhile.
 *
 * Keys: the original reads the BIOS keyboard (getkey 0x5fad, kbhit drain
 * 0x4137).  The pad stands in: the D-pad is the arrows (repeating when
 * held, as a keyboard does), A and B are ENTER, C and OPTION are ESC.
 */
#include <string.h>
#include "shared.h"
#include "front.h"
#include "mem_map.h"

#define HW16(a) (*(volatile int16_t *)(uintptr_t)(a))
#define HW32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define FRONT_VIEW VIEW0
#define SCR ((uint8_t *)(uintptr_t)FRONT_VIEW)

sr_cfg cfg;
int fe_road;

/* ---- the blob (tools/sr2jag.py front_blob) ------------------------------- */
typedef struct {
    uint16_t ofs, w, h;
    const uint8_t *px;
} pic_t;

typedef struct {
    const uint8_t *rgb;
    uint16_t first, count;
} pdesc;

#define FB ((const uint8_t *)(uintptr_t)ROM_FRONT)
static uint32_t n_pics, n_cmaps, n_anim;

static pic_t pic(int i)
{
    const uint8_t *e = FB + 16 + 12 * (uint32_t)i;
    pic_t p = { get16(e), get16(e + 2), get16(e + 4), FB + get32(e + 8) };
    return p;
}

static pdesc cmap(int i)
{
    const uint8_t *e = FB + 16 + 12 * n_pics + 8 * (uint32_t)i;
    pdesc d = { FB + get32(e + 4), get16(e), get16(e + 2) };
    return d;
}

static const uint8_t *anim_rec(int i)
{
    return FB + 16 + 12 * n_pics + 8 * n_cmaps + 4 * (uint32_t)i;
}

/* ---- time and keys ---------------------------------------------------------
 * fe_n is ds:0x160c, the ticks since the code last zeroed it.  Key events go
 * into a queue (the BIOS buffer); getkey takes one, drain (fn_4137) empties
 * it and, while `armed` (ds:0xaf42, the intro), turns any key into `abort_`
 * (ds:0x54ac), which cuts every wait and fade short. */
enum { K_NONE, K_ENTER, K_ESC, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OTHER };

static uint32_t fe_n;
static int armed, abort_;
static uint8_t kq[15];
static int kq_n;
static uint16_t kprev;
static int rep_key, rep_t;

#define PADB(n) (1u << (n))
#define REP_DELAY 18            /* ~0.5 s, the PC's typematic default */
#define REP_EVERY 3             /* ~12 a second */

static void kpush(int k)
{
    if (kq_n < (int)sizeof kq)
        kq[kq_n++] = (uint8_t)k;
}

static void keys_scan(void)
{
    static const uint8_t dir_bit[4] = { PAD_B_UP, PAD_B_DOWN, PAD_B_LEFT, PAD_B_RIGHT };
    static const uint8_t dir_key[4] = { K_UP, K_DOWN, K_LEFT, K_RIGHT };
    uint16_t pad = (uint16_t)HW16(PAD_STATE);
    uint16_t edge = pad & (uint16_t)~kprev;
    kprev = pad;
    for (int i = 0; i < 4; i++)
        if (edge & PADB(dir_bit[i])) {
            kpush(dir_key[i]);
            rep_key = i + 1;
            rep_t = 0;
        }
    if (edge & (PADB(PAD_B_A) | PADB(PAD_B_B)))
        kpush(K_ENTER);
    if (edge & (PADB(PAD_B_C) | PADB(PAD_B_OPT)))
        kpush(K_ESC);
    if (edge & PADB(PAD_B_PAUSE))
        kpush(K_OTHER);
    if (rep_key && (pad & PADB(dir_bit[rep_key - 1]))) {
        if (++rep_t >= REP_DELAY && (rep_t - REP_DELAY) % REP_EVERY == 0)
            kpush(dir_key[rep_key - 1]);
    } else {
        rep_key = 0;
    }
}

static int ee_pending;                  /* an EEPROM line for COM8 (below) */
static void ee_send(void);

/* One game tick: STOP until the video interrupt has counted one (at $2700
 * for the test, so one counted in between cannot be slept through), take
 * it, read the pad (RMAC's ReadPad, through PAD_FN), feed Jerry. */
static void fe_tick(void)
{
    for (;;) {
        __asm__ volatile("move.w #0x2700,%%sr" ::: "memory");
        if (HW32(GAME_TICKS) != HW32(GAME_DONE))
            break;
        __asm__ volatile("stop #0x2000" ::: "memory");
    }
    __asm__ volatile("move.w #0x2000,%%sr" ::: "memory");
    HW32(GAME_DONE) = HW32(GAME_DONE) + 1;
    fe_n++;
    ((void (*)(void))(uintptr_t)HW32(PAD_FN))();
    keys_scan();
    audio_post();
    if (ee_pending)
        ee_send();
}

static void drain(void)                 /* fn_4137 */
{
    if (kq_n) {
        if (armed)
            abort_ = 1;
        kq_n = 0;
    }
}

static int getkey(void)                 /* BIOS 0x5fad: waits for a key */
{
    while (!kq_n)
        fe_tick();
    int k = kq[0];
    kq_n--;
    for (int i = 0; i < kq_n; i++)
        kq[i] = kq[i + 1];
    return k;
}

void fe_flush(void)
{
    kprev = (uint16_t)HW16(PAD_STATE);
    kq_n = 0;
    rep_key = 0;
}

void fe_wait(int n)                     /* fn_443d */
{
    fe_n = 0;
    while (fe_n < (uint32_t)n && !abort_) {
        drain();
        if (abort_)
            break;
        fe_tick();
    }
}

/* ---- the DAC --------------------------------------------------------------
 * dac = what the VGA DAC holds; dac_publish turns it into Jaguar RGB16
 * (red 15-11, BLUE 10-6, green 5-0; tools/sr2jag.py rgb16) for the video
 * interrupt to load in the blanking. */
static uint8_t dac[768];
static uint8_t mpal[768];               /* the menu palette, ds:0x5182 */
static const uint8_t black[768];
static uint8_t white[768];
static uint8_t to5[64];

static void dac_publish(void)
{
    uint16_t *out = (uint16_t *)(uintptr_t)FADE_PAL;
    const uint8_t *d = dac;
    for (int i = 0; i < 256; i++, d += 3)
        out[i] = (uint16_t)(to5[d[0]] << 11 | to5[d[2]] << 6 | d[1]);
    HW32(FADE_SEQ) = HW32(FADE_SEQ) + 1;
}

static void set_dac(pdesc p)            /* fn_4260 */
{
    memcpy(dac + 3 * p.first, p.rgb, 3u * p.count);
    dac_publish();
}

static void pal_load(pdesc p)           /* fn_3f75 / fn_4469: into ds:0x5182 */
{
    memcpy(mpal + 3 * p.first, p.rgb, 3u * p.count);
}

/* fn_4315: from -> to over `steps` ticks, on the DAC range of `from`.
 * t = 100 * ticks / steps, each gun from + (to - from) * t / 100 truncated
 * toward zero (idiv), recomputed from the two ends each pass; keys are
 * drained every pass.  t reaches 100 after `steps` ticks, or at once when
 * cut short. */
static void fade(pdesc from, pdesc to, int steps)
{
    fe_n = 0;
    for (;;) {
        int t = steps ? (int)(100u * fe_n / (uint32_t)steps) : 100;
        if (t > 100 || abort_)
            t = 100;
        uint8_t *o = dac + 3 * from.first;
        for (int i = 0; i < 3 * from.count; i++) {
            int f = from.rgb[i];
            o[i] = (uint8_t)(f + (to.rgb[i] - f) * t / 100);
        }
        dac_publish();
        drain();
        if (t == 100)
            return;
        fe_tick();
    }
}

/* fn_4b72: a whole palette from black, or to black. */
void fe_fade_full(const uint8_t *pal, int in, int steps)
{
    pdesc p = { pal, 0, 256 }, b = { black, 0, 256 };
    if (in)
        fade(b, p, steps);
    else
        fade(p, b, steps);
}

/* ---- pictures ---------------------------------------------------------------
 * fn_4162 sets the masked blitter up: bg (where "background" pixels come
 * from, same offsets as the destination), dst, and two thresholds; fn_4184
 * draws one row: `skl` background bytes, then w - skl - skr source pixels,
 * then `skr` background bytes.  Per source pixel p (unsigned):
 *     p <  thr          the destination is left alone
 *     p <  minc         the background's pixel is written
 *     else              p is written
 * (0x41cc: the threshold is patched into a cmp; ds:0xaf4a is minc.)  So
 * (minc 1, thr 0) draws a picture whose zeros show the background; (0xff,
 * 1) erases a picture back to the background; (0, 0) is opaque. */
static uint8_t *bl_bg, *bl_dst;
static uint8_t bl_minc, bl_thr;
static int bl_w;

static void blit_set(const uint8_t *bg, uint8_t *dst, int minc, int thr)
{
    bl_bg = (uint8_t *)(uintptr_t)bg;
    bl_dst = dst;
    bl_minc = (uint8_t)minc;
    bl_thr = (uint8_t)thr;
}

/* Source pixels for x in [x0, x1) of a row at dofs, source row `src`
 * indexed by the same x. */
static void span(uint32_t dofs, const uint8_t *src, int x0, int x1)
{
    uint8_t *d = bl_dst + dofs;
    const uint8_t *b = bl_bg + dofs;
    uint8_t thr = bl_thr, minc = bl_minc;
    for (int x = x0; x < x1; x++) {
        uint8_t p = src[x];
        if (p >= thr)
            d[x] = p < minc ? b[x] : p;
    }
}

static void bg_bytes(uint32_t dofs, int x0, int x1)
{
    memcpy(bl_dst + dofs + x0, bl_bg + dofs + x0, (size_t)(x1 - x0));
}

static void row4184(uint32_t dofs, const uint8_t *src, int skl, int skr)
{
    if (skl)
        bg_bytes(dofs, 0, skl);
    span(dofs + (uint32_t)skl, src, 0, bl_w - skl - skr);
    if (skr)
        bg_bytes(dofs, bl_w - skr, bl_w);
}

static void blit_pic(const pic_t *p)    /* fn_41e5 */
{
    bl_w = p->w;
    for (int y = 0; y < p->h; y++)
        row4184(p->ofs + (uint32_t)y * 320, p->px + (uint32_t)y * p->w, 0, 0);
}

/* n bytes, by longs when both ends allow it.  In assembly: from the C loop
 * m68k-elf-gcc 15.2 made "move.l (a2)+,(0,a2,d0.l)" with d0 = dst - src,
 * and the 68000 increments a2 BEFORE it forms the destination, so every
 * long landed 4 bytes late (the road map's text 4 pixels right of its frame,
 * the menu's title buffer off by 4: seen on hardware, session 5). */
static void cpy(uint8_t *d, const uint8_t *s, uint32_t n)
{
    if ((((uintptr_t)d | (uintptr_t)s | n) & 3) == 0) {
        uint32_t k = n >> 2;
        if (k)
            __asm__ volatile("1:\n\tmove.l (%1)+,(%0)+\n\tsubq.l #1,%2\n\tbne.s 1b"
                             : "+a"(d), "+a"(s), "+d"(k) : : "memory");
    } else {
        memcpy(d, s, n);
    }
}

static void blit_opaque(const pic_t *p, uint8_t *dst)   /* fn_4293 */
{
    if (p->w == 320)
        cpy(dst + p->ofs, p->px, 320u * p->h);
    else
        for (int y = 0; y < p->h; y++)
            cpy(dst + p->ofs + (uint32_t)y * 320, p->px + (uint32_t)y * p->w, p->w);
}

/* ---- the text (fn_450a / fn_44a2 / fn_3c36) ---------------------------------
 * The original draws with the VGA BIOS's 8x8 font (int 10h 1130h), only the
 * set bits, 8 pixels a character.  That font is the PC's, not SkyRoads', so
 * these are our own glyphs in its spirit, for the two strings the game
 * writes ("Road Completed", "The End"). */
static const struct { char c; uint8_t g[8]; } glyphs[] = {
    { 'R', { 0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00 } },
    { 'o', { 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00 } },
    { 'a', { 0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00 } },
    { 'd', { 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00 } },
    { 'C', { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 } },
    { 'm', { 0x00, 0x00, 0x66, 0x7F, 0x7F, 0x6B, 0x63, 0x00 } },
    { 'p', { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60 } },
    { 'l', { 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 } },
    { 'e', { 0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00 } },
    { 't', { 0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x0E, 0x00 } },
    { 'T', { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 } },
    { 'h', { 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 } },
    { 'E', { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 } },
    { 'n', { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 } },
};

void fe_text(uint8_t *screen, int x, int y, const char *s, uint8_t colour)
{
    for (; *s; s++, x += 8)
        for (unsigned k = 0; k < sizeof glyphs / sizeof glyphs[0]; k++)
            if (glyphs[k].c == *s) {
                for (int r = 0; r < 8; r++) {
                    uint8_t *d = screen + (uint32_t)(y + r) * 320 + x;
                    uint8_t g = glyphs[k].g[r];
                    for (int c = 0; c < 8; c++, g <<= 1)
                        if (g & 0x80)
                            d[c] = colour;
                }
                break;
            }
}

/* ---- the screen --------------------------------------------------------------- */
void fe_wait_shown(void)
{
    for (;;) {
        __asm__ volatile("move.w #0x2700,%%sr" ::: "memory");
        if (HW32(ROAD_SHOWN) == HW32(ROAD_SEQ))
            break;
        __asm__ volatile("stop #0x2000" ::: "memory");
    }
    __asm__ volatile("move.w #0x2000,%%sr" ::: "memory");
}

void fe_show(void)
{
    if (HW32(ROAD_VIEW) != FRONT_VIEW) {
        HW32(ROAD_VIEW) = FRONT_VIEW;
        HW32(ROAD_SEQ) = HW32(ROAD_SEQ) + 1;
    }
    fe_wait_shown();
    fe_flush();
}

/* c_init, interrupts still off: a black DAC and an empty FRONT_VIEW as the
 * frame the OP list starts on.  0 if front.bin is not the one this code
 * was written for. */
int fe_init(void)
{
    if (get32(FB) != 0x46524E54u)
        return 0;
    n_pics = get32(FB + 4);
    n_cmaps = get32(FB + 8);
    n_anim = get32(FB + 12);
    if (n_cmaps != C_COUNT || n_pics != P_ANIM0 + n_anim)
        return 0;
    for (int c = 0; c < 64; c++)
        to5[c] = (uint8_t)((c * 31 + 31) / 63);
    memset(white, 0x3F, sizeof white);
    memset(dac, 0, sizeof dac);
    dac_publish();
    memset(SCR, 0, 320u * 200u);
    HW32(ROAD_VIEW) = FRONT_VIEW;
    HW32(ROAD_SEQ) = HW32(ROAD_SEQ) + 1;
    return 1;
}

/* ---- the intro (fn_4575) -------------------------------------------------------
 * Song 0; the title under ANIM.LZS's palette, faded in; INTRO.SND after 24
 * ticks; the animation (a frame every 2 ticks, records only: frames without
 * one take no time); 72 ticks; the logo revealed in 18 ticks, even rows
 * from the left and odd ones from the right, under colour map B; a flash to
 * white and down to C; five captions (scenes 2..6: the loop starts at 2)
 * faded in and out over the title; a fade to black.  Returns whether a key
 * cut it short, in which case it ends at once on title and logo, no fade. */
static void logo_wipe(void)
{
    pic_t lg = pic(P_LOGO);
    bl_w = 320;                         /* ds:0x961e = 0x140 (0x480c) */
    fe_n = 0;
    int prev = -1;
    for (;;) {
        int col = 319 - (int)(319u * fe_n / 18u);
        if (col < 0 || abort_)
            col = 0;
        for (int y = 0; y < lg.h; y += 2) {
            uint32_t d = lg.ofs + (uint32_t)y * 320;
            const uint8_t *s = lg.px + (uint32_t)y * lg.w;
            if (prev < 0) {             /* the first pass: whole rows */
                row4184(d, s, 0, col);
                row4184(d + 320, s + 320 + col, col, 0);
            } else if (col < prev) {    /* then only what is newly revealed */
                span(d, s, 320 - prev, 320 - col);
                span(d + 320, s + 320, col, prev);
            }
        }
        prev = col;
        if (col == 0)
            return;
        fe_tick();
    }
}

int fe_intro(void)
{
    int last = -1;
    pic_t title = pic(P_TITLE);
    music(0);
    abort_ = 0;
    armed = 1;
    memset(mpal, 0, sizeof mpal);
    fe_fade_full(mpal, 0, 0);           /* black at once */
    pal_load(cmap(C_ANIM));
    blit_opaque(&title, SCR);
    fe_fade_full(mpal, 1, 36);
    fe_wait(24);
    if (!abort_)
        sfx_voice();
    fe_wait(37);
    if (!abort_) {
        fe_n = 0;
        for (uint32_t i = 0; i < n_anim; i++) {
            const uint8_t *r = anim_rec((int)i);
            if (get16(r) != last) {
                while (fe_n < 2)
                    fe_tick();
                fe_n = 0;
                last = get16(r);
            }
            pic_t p = pic(get16(r + 2));
            blit_opaque(&p, SCR);
            drain();
            if (abort_) {
                blit_opaque(&title, SCR);
                break;
            }
        }
    }
    blit_set(title.px, SCR, 1, 0);
    fe_wait(72);
    set_dac(cmap(C_B));
    logo_wipe();
    pdesc b = cmap(C_B), c = cmap(C_C), w = { white, b.first, b.count };
    if (!abort_) {
        fade(b, w, 5);
        fe_wait(9);
        fade(w, c, 70);
    } else {
        fade(b, c, 5);
    }
    for (int k = 2; k < 7 && !abort_; k++) {
        pdesc e = cmap(C_E0 + k), g = cmap(C_G0 + k);
        pic_t f = pic(P_SCENE0 + k);
        set_dac(e);
        blit_set(title.px, SCR, 1, 0);
        blit_pic(&f);
        fade(e, g, 50);
        fe_wait(50);
        fade(g, e, 50);
        bl_minc = 0xFF;                 /* ds:0xaf4a: erase it */
        blit_pic(&f);
    }
    int r = abort_;
    abort_ = 0;
    armed = 0;
    if (!r) {
        pal_load(cmap(C_A));
        pal_load(cmap(C_C));
        fe_fade_full(mpal, 0, 36);
    }
    return r;
}

/* ---- settings (fn_4c04) ----------------------------------------------------------
 * Items 0..4 are the cursor's frames (keyboard, joystick, mouse, speaker,
 * Zzz), 5..9 the marks of the chosen ones (fn_4ae2).  fn_4aaf(i, minc)
 * draws item i (minc 1) or erases it to the background (0xff). */
static void set_item(int i, int minc)   /* fn_4aaf */
{
    pic_t bg = pic(P_SET_BG), p = pic(P_SET_ITEM0 + i);
    blit_set(bg.px, SCR, minc, 1);
    blit_pic(&p);
}

static void set_marks(void)             /* fn_4ae2 */
{
    for (int i = 0; i < 5; i++)
        set_item(i + 5, (i == cfg.control || i == cfg.sound_off + 3) ? 1 : 0xFF);
}

/* ---- skyroads.cfg in the cartridge EEPROM (src/eeprom.s) -----------------
 * The file is 33 words (fn_571b / fn_5770): a checksum, then control, sound
 * off and the 30 completion counters.  In the EEPROM's 64 words:
 *     0  'SR', ours: an unwritten chip reads $FFFF
 *     1  the checksum fn_56c3 MEANT: sum of (word i XOR i), i = 1..32
 *        (the EXE advances its pointer once, before the loop, so it sums
 *        (control XOR i) 32 times and guards nothing else; the layout here
 *        is ours anyway)
 *     2  control   3  sound off   4..33  done[0..29]
 * A wrong ID, checksum or setting = a fresh file of zeros, as the original
 * does with an invalid one.  Saving writes only the words that changed,
 * payload first, the checksum and the ID last: a save cut short reads back
 * as invalid, never as a mixture. */
#define EE_ID    0x5352u
#define EE_WORDS 34
static uint16_t ee_img[EE_WORDS];       /* what the chip holds */
uint32_t ee_errors;                     /* writes that never verified */

/* The EEPROM's words to COM8 (tools/comlog.py listen): what was read at boot
 * (sent at the first tick: GdSendC is not for interrupts-off c_init) and what
 * each save wrote.  An instrument for the first EEPROM on this hardware. */
static char ee_line[200];
static int ee_pending;

static void ee_send(void)
{
    uint32_t fn = HW32(GD_SEND_FN);
    if (fn)
        ((void (*)(const char *))(uintptr_t)fn)(ee_line);
    ee_pending = 0;
}

static char *hex4(char *o, uint32_t v)
{
    *o++ = ' ';
    for (int i = 3; i >= 0; i--)
        *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return o;
}

static void ee_format(const char *what, uint32_t a, uint32_t b)
{
    char *o = ee_line;
    while (*what)
        *o++ = *what++;
    o = hex4(o, a);
    o = hex4(o, b);
    *o++ = ' ';
    *o++ = '|';
    for (int i = 0; i < EE_WORDS; i++)
        o = hex4(o, ee_img[i]);
    *o = 0;
}

static uint32_t ee_read(uint32_t w)
{
    return ((uint32_t (*)(uint32_t))(uintptr_t)HW32(EE_READ_FN))(w);
}

static int ee_write(uint32_t w, uint32_t v)
{
    return ((int (*)(uint32_t, uint32_t))(uintptr_t)HW32(EE_WRITE_FN))(w, v);
}

static void cfg_image(uint16_t *w)
{
    w[0] = EE_ID;
    w[2] = cfg.control;
    w[3] = cfg.sound_off;
    for (int i = 0; i < 30; i++)
        w[4 + i] = cfg.done[i];
    uint16_t sum = 0;
    for (int i = 1; i <= 32; i++)
        sum = (uint16_t)(sum + (w[1 + i] ^ i));
    w[1] = sum;
}

void fe_cfg_load(void)
{
    uint16_t w[EE_WORDS];
    for (int i = 0; i < EE_WORDS; i++)
        ee_img[i] = (uint16_t)ee_read((uint32_t)i);
    memset(&cfg, 0, sizeof cfg);
    cfg.control = ee_img[2];
    cfg.sound_off = ee_img[3];
    for (int i = 0; i < 30; i++)
        cfg.done[i] = ee_img[4 + i];
    cfg_image(w);
    int ok = !(ee_img[0] != EE_ID || ee_img[1] != w[1] || cfg.control > 2 || cfg.sound_off > 1);
    if (!ok)
        memset(&cfg, 0, sizeof cfg);
    ee_format("EE load ok/sum", (uint32_t)ok, w[1]);
    ee_pending = 1;
}

void fe_cfg_save(void)                  /* fn_5770 */
{
    uint16_t w[EE_WORDS];
    uint32_t n = 0, e0 = ee_errors;
    cfg_image(w);
    for (int i = EE_WORDS - 1; i >= 0; i--) {
        if (w[i] == ee_img[i])
            continue;
        n++;
        for (int k = 0; k < 2; k++) {   /* a second try, as Jaguar Doom */
            ee_write((uint32_t)i, w[i]);
            if (ee_read((uint32_t)i) == w[i])
                break;
            if (k)
                ee_errors++;
        }
        ee_img[i] = w[i];
    }
    /* what the chip holds now, read back word by word */
    for (int i = 0; i < EE_WORDS; i++)
        ee_img[i] = (uint16_t)ee_read((uint32_t)i);
    ee_format("EE save n/err", n, ee_errors - e0);
    ee_send();
}

static void settings(void)
{
    int sel = 0, prev = 0;
    pic_t bg = pic(P_SET_BG);
    pal_load(cmap(C_SET1));
    pal_load(cmap(C_SET2));
    blit_opaque(&bg, SCR);
    set_marks();
    set_item(0, 1);
    fe_fade_full(mpal, 1, 36);
    for (;;) {
        if (sel != prev) {
            set_item(prev, 0xFF);
            prev = sel;
            set_item(sel, 1);
        }
        int k = getkey();
        if (k == K_ENTER) {
            if (sel <= 2)
                cfg.control = (uint16_t)sel;
            else
                cfg.sound_off = (uint16_t)(sel - 3);
            if (cfg.sound_off)
                music_stop();
            else
                music(1);
            set_marks();
        } else if (k == K_ESC) {
            break;
        } else if (k == K_UP) {
            if (sel == 3)
                sel = 0;
            if (sel == 4)
                sel = 1;
        } else if (k == K_LEFT) {
            if (sel > 0)
                sel--;
        } else if (k == K_RIGHT) {
            if (sel < 4)
                sel++;
        } else if (k == K_DOWN) {
            if (sel == 0)
                sel = 3;
            else if (sel < 3)
                sel = 4;
        }
    }
    fe_cfg_save();
}

/* ---- help (fn_4e12 / fn_4dac): page 1, and page 2 unless ESC; the file's
 * third page is never shown. */
static int help_page(int k)
{
    pic_t p = pic(P_HELP0 + k);
    pal_load(cmap(C_HELP0 + k));
    blit_opaque(&p, SCR);
    fe_fade_full(mpal, 1, 36);
    int key = getkey();
    fe_fade_full(mpal, 0, 36);
    return key != K_ESC;
}

/* ---- the main menu (fn_4e36) ---------------------------------------------------
 * The title with the logo composed into it, the menu's panel (scene 0)
 * faded from E0 to G0, then the three boxes (play, settings, help) drawn
 * over it.  `fade_in` 0: straight after an interrupted intro, whose screen
 * is still up: only the boxes' three colours go to the DAC.  ESC leaves to
 * DOS in the original (0x46): nothing here. */
void fe_mainmenu(int fade_in)
{
    uint8_t *tb = (uint8_t *)(uintptr_t)TITLE_BUF;
    pic_t title = pic(P_TITLE), logo = pic(P_LOGO), panel = pic(P_SCENE0);
    music(1);
    pal_load(cmap(C_MENU));
    pal_load(cmap(C_A));
    cpy(tb, title.px, 320u * 200u);
    pal_load(cmap(C_B));
    pal_load(cmap(C_C));
    blit_set(tb, tb, 1, 0);
    blit_pic(&logo);
    if (!fade_in) {
        memcpy(dac + 3 * 0xBE, mpal + 3 * 0xBE, 9);
        dac_publish();
    } else {
        cpy(SCR, tb, 320u * 200u);
        fe_fade_full(mpal, 1, 36);
    }
    set_dac(cmap(C_E0));
    blit_set(0, SCR, 0, 1);
    blit_pic(&panel);
    fade(cmap(C_E0), cmap(C_G0), 50);
    int si = 0;
    for (;;) {
        pic_t box = pic(P_BOX0 + si);
        blit_set(tb, SCR, 1, 0);
        blit_pic(&box);
        int k = getkey();
        if (k == K_UP) {
            if (si > 0)
                si--;
        } else if (k == K_DOWN) {
            if (si < 2)
                si++;
        } else if (k == K_ENTER) {
            fe_fade_full(mpal, 0, 36);
            if (si == 0)
                return;
            if (si == 1) {
                settings();
                fe_fade_full(mpal, 0, 36);
            } else if (help_page(0)) {
                help_page(1);
            }
            cpy(SCR, tb, 320u * 200u);
            fe_fade_full(mpal, 1, 36);
        }
    }
}

/* ---- the road map (fn_5164) ----------------------------------------------------------
 * The map with a tick per completion (at most 7) drawn INTO the picture;
 * the selection is a 48x9 frame of colour 1 around the road's name
 * (fn_5064), erased back to the picture when it moves.  Up/down step one
 * road, left/right fifteen (left from the first column goes to road 1,
 * right past the end is clamped to 30); the selection survives between
 * visits (ds:0x933e, which a completed road advances). */
static uint8_t frame_px[48 * 9];

static void road_frame(int rd, int on)  /* fn_5064 */
{
    uint8_t *bg = (uint8_t *)(uintptr_t)MENU_BG;
    memset(frame_px, 1, sizeof frame_px);
    for (int i = 0x31; i < 0x180; i += 0x30)
        memset(frame_px + i, 0, 0x2E);
    pic_t f = { (uint16_t)(0xF3E + (rd % 3) * 9 * 320 + (rd / 3 % 5) * 39 * 320
                           + (rd >= 15 ? 0xA0 : 0)), 48, 9, frame_px };
    blit_set(bg, SCR, on ? 0 : 0xFF, 1);
    blit_pic(&f);
}

int fe_gomenu(void)
{
    uint8_t *bg = (uint8_t *)(uintptr_t)MENU_BG;
    pic_t map = pic(P_GO_BG), tick = pic(P_GO_TICK);
    int prev = 1000, r;
    music(1);
    pal_load(cmap(C_GO));
    cpy(bg, map.px, 320u * 200u);
    pal_load(cmap(C_GO2));
    blit_set(0, bg, 0, 0);
    for (int si = 0; si < 30; si++) {
        int di = si % 15;
        uint32_t ofs = 0x11F0 + (uint32_t)(di % 3) * 9 * 320 + (uint32_t)(di / 3) * 39 * 320
                       + (si >= 15 ? 0xA0 : 0);
        for (int t = 0; t < cfg.done[si] && t < 7; t++, ofs += 7) {
            tick.ofs = (uint16_t)ofs;
            blit_pic(&tick);
        }
    }
    cpy(SCR, bg, 320u * 200u);
    fe_fade_full(mpal, 1, 36);
    for (;;) {
        if (fe_road >= 30)
            fe_road = 29;
        if (prev != fe_road) {
            if (prev < 1000)
                road_frame(prev, 0);
            prev = fe_road;
            road_frame(fe_road, 1);
        }
        int k = getkey();
        if (k == K_ENTER) {
            r = 0;
            break;
        } else if (k == K_ESC) {
            r = 1;
            break;
        } else if (k == K_UP) {
            if (fe_road > 0)
                fe_road--;
        } else if (k == K_LEFT) {
            fe_road = fe_road >= 15 ? fe_road - 15 : 0;
        } else if (k == K_RIGHT) {
            fe_road += 15;
        } else if (k == K_DOWN) {
            fe_road++;
        }
    }
    fe_fade_full(mpal, 0, 36);
    return r;
}
