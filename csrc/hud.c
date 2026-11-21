/* See hud.h.  Constants and colours are skyroads-port's hud.c. */
#include "hud.h"

void hud_pristine(uint8_t *pristine, const uint8_t *world, const uint8_t *dash)
{
    for (int i = 0; i < 320 * 138; i++)
        pristine[i] = world[i];
    for (int i = 320 * 138; i < 320 * 200; i++)
        pristine[i] = 0;
    for (int y = 0; y < 71; y++)
        for (int x = 0; x < 320; x++) {
            uint8_t c = dash[y * 320 + x];
            if (c)
                pristine[(129 + y) * 320 + x] = c;
        }
}

static const uint8_t *seg(const uint8_t *hud, int i)
{
    return hud + get32(hud + 4 * i);
}

static void draw_cells(uint8_t *view, const uint8_t *pristine, const uint8_t *r,
                       uint8_t c1, uint8_t c2, int lit)
{
    int x0 = get16(r), y0 = get16(r + 2), w = get16(r + 4), h = get16(r + 6);
    const uint8_t *cells = r + 8;
    int32_t at = (int32_t)(int16_t)y0 * 320 + x0;
    uint8_t *v = view + at;
    const uint8_t *pr = pristine + at;
    for (int y = 0; y < h; y++, cells += w, v += 320, pr += 320)
        for (int x = 0; x < w; x++) {
            uint8_t c = cells[x];
            if (c)
                v[x] = lit ? (c == 1 ? c1 : c2) : pr[x];
        }
}

static void draw_digit(uint8_t *view, const uint8_t *digits, int x, int y, int d)
{
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 4; i++) {
            uint8_t v = digits[d * 20 + j * 4 + i];
            if (v)
                view[(y + j) * 320 + x + i] = v == 1 ? 0x61 : 0x62;
        }
}

void hud_number(uint8_t *view, const uint8_t *hud, int x, int y, uint32_t v, int n,
                int scale, uint8_t fg, uint8_t bg)
{
    /* The EXE's digit stencils (ds:0x13c) are NOT glyphs: they are what to
     * switch OFF over an LCD "8" the dashboard art already shows (fn_0fc6 /
     * fn_0eb5 / fn_41e5: 5 rows of 4, 1 and 2 = the two unlit tones, 0 =
     * leave the art).  So a digit is the 8's cells minus its stencil.
     * No divisions and no multiplications in the loops: on the 68000 a
     * 32-bit multiply is a library call, and with one per cell this
     * instrument cost ~800 half-lines a frame (M3, hardware: 9 fps shown,
     * with 4 ticks of catch-up per frame).  Row pointers instead. */
    static const uint8_t eight[20] = { 1,1,1,1, 1,0,0,1, 1,1,1,1, 1,0,0,1, 1,1,1,1 };
    const uint8_t *digits = seg(hud, HUD_N_SPEED + HUD_N_OXY + HUD_N_FUL);
    int cw = 5 * scale, h = 5 * scale, w = n * cw + scale;
    int32_t band = (int32_t)(int16_t)(320 * scale);
    uint8_t *org = view + (int32_t)(int16_t)y * 320 + x;
    uint8_t *row = org - band - scale;
    for (int j = -scale; j < h + scale; j++, row += 320)
        for (int i = 0; i < w; i++)
            row[i] = bg;
    uint8_t *dig = org + (n - 1) * cw;
    for (int k = n - 1; k >= 0; k--, v /= 10, dig -= cw) {
        const uint8_t *d = digits + 20 * (v % 10);
        uint8_t *crow = dig;
        for (int r = 0; r < 5; r++, crow += band) {
            uint8_t *cell = crow;
            for (int c = 0; c < 4; c++, cell += scale) {
                if (!eight[r * 4 + c] || d[r * 4 + c])
                    continue;
                uint8_t *p = cell;
                for (int sy = 0; sy < scale; sy++, p += 320)
                    for (int sx = 0; sx < scale; sx++)
                        p[sx] = fg;
            }
        }
    }
}

/* The gauges' values, as the original derives them from the play state. */
static void hud_values(const sr_play *p, hud_state *v)
{
    int32_t disp = p->speed - p->ap_delta;        /* without the autopilot's fix */
    if (disp < 0) disp = 0;
    int sp = (int)(disp / 0x141);
    v->speed = (int16_t)(sp > HUD_N_SPEED ? HUD_N_SPEED : sp);
    int oxy = (p->oxy + 0xbb7) / 0xbb8, ful = (p->fuel + 0xbb7) / 0xbb8;
    v->oxy = (int16_t)(oxy > 10 ? 10 : oxy);
    v->ful = (int16_t)(ful > 10 ? 10 : ful);
    int rows = p->road->rows, steps = 0;
    if (rows > 3) {
        int32_t denom = (int32_t)(((int64_t)(rows - 3) << 16) / 30);
        if (denom > 0)
            steps = (int)(((int64_t)p->z - 0x30000) / denom);
    }
    if (steps < 0) steps = 0;
    if (steps > 29) steps = 29;
    v->steps = (int16_t)steps;
    v->lamp = (int16_t)(p->ap_light ? 1 : 0);
}

/* Segments [from, to) of one gauge, lit below n. */
static void gauge(uint8_t *view, const uint8_t *pristine, const uint8_t *hud,
                  int first, int from, int to, int n)
{
    for (int i = from; i < to; i++)
        draw_cells(view, pristine, seg(hud, first + i), 0x5e, 0x5f, i < n);
}

/* Progress columns [from, to): each filled up its slot from y = 143. */
static void progress(uint8_t *view, const uint8_t *pristine, int from, int to)
{
    for (int i = from; i < to; i++) {
        int x = 0x2a + i;
        uint8_t slot = pristine[143 * 320 + x];
        for (int yy = 143; yy >= 138 && pristine[yy * 320 + x] == slot; yy--)
            view[yy * 320 + x] = 0x60;
    }
}

/* Jump-o-master lamp: 26x5 at (203,156); its rectangle from the pristine
 * screen first, so the other stamp's pixels do not stay behind. */
static void lamp(uint8_t *view, const uint8_t *pristine, const uint8_t *hud, int on)
{
    const uint8_t *st = seg(hud, HUD_N_SPEED + HUD_N_OXY + HUD_N_FUL + 1) + 130 * on;
    for (int j = 0; j < 5; j++) {
        uint8_t *v = view + (156 + j) * 320 + 203;
        const uint8_t *pr = pristine + (156 + j) * 320 + 203;
        for (int i = 0; i < 26; i++)
            v[i] = st[j * 26 + i] ? 0x62 : pr[i];
    }
}

/* GRAV-O-METER: (g - 3) * 100 in 4 digits at (0x60, 0x9c), leading zeros
 * suppressed; fixed for a road. */
static void gravity(uint8_t *view, const uint8_t *hud, const sr_play *p)
{
    const uint8_t *digits = seg(hud, HUD_N_SPEED + HUD_N_OXY + HUD_N_FUL);
    int value = ((int)p->road->gravity - 3) * 100;
    if (value < 0) value = 0;
    static const int div[4] = { 1, 10, 100, 1000 };
    for (int si = 0; si < 4; si++) {
        draw_digit(view, digits, 0x60 + (3 - si) * 5, 0x9c, (value / div[si]) % 10);
        if (value / div[si] / 10 == 0)
            break;
    }
}

void hud_draw(uint8_t *view, const uint8_t *pristine, const uint8_t *hud,
              const sr_play *p)
{
    hud_state v;
    hud_values(p, &v);
    gauge(view, pristine, hud, 0, 0, HUD_N_SPEED, v.speed);
    for (int i = 0; i < 10; i++) {
        gauge(view, pristine, hud, HUD_N_SPEED, i, i + 1, v.oxy);
        gauge(view, pristine, hud, HUD_N_SPEED + HUD_N_OXY, i, i + 1, v.ful);
    }
    progress(view, pristine, 0, v.steps);
    lamp(view, pristine, hud, v.lamp);
    gravity(view, hud, p);
}

void hud_update(uint8_t *view, const uint8_t *pristine, const uint8_t *hud,
                const sr_play *p, hud_state *st)
{
    hud_state v;
    hud_values(p, &v);
    if (!st->valid || v.steps < st->steps) {
        /* everything, from the pristine dashboard rows */
        for (int i = HUD_ROW0 * 320; i < 320 * 200; i++)
            view[i] = pristine[i];
        hud_draw(view, pristine, hud, p);
        *st = v;
        st->valid = 1;
        return;
    }
    if (v.speed != st->speed) {
        int lo = v.speed < st->speed ? v.speed : st->speed;
        int hi = v.speed < st->speed ? st->speed : v.speed;
        gauge(view, pristine, hud, 0, lo, hi, v.speed);
    }
    if (v.oxy != st->oxy) {
        int lo = v.oxy < st->oxy ? v.oxy : st->oxy, hi = v.oxy < st->oxy ? st->oxy : v.oxy;
        gauge(view, pristine, hud, HUD_N_SPEED, lo, hi, v.oxy);
    }
    if (v.ful != st->ful) {
        int lo = v.ful < st->ful ? v.ful : st->ful, hi = v.ful < st->ful ? st->ful : v.ful;
        gauge(view, pristine, hud, HUD_N_SPEED + HUD_N_OXY, lo, hi, v.ful);
    }
    if (v.steps > st->steps)
        progress(view, pristine, st->steps, v.steps);
    if (v.lamp != st->lamp)
        lamp(view, pristine, hud, v.lamp);
    v.valid = 1;
    *st = v;
}
