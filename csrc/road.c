/* The road's composers: see road.h.  Function names and EXE addresses follow
 * skyroads-port's render.c, which this file transcribes into commands. */
#include "road.h"

typedef struct {
    const road_ctx *c;
    const uint8_t *obj;       /* phase object in trekg */
    const uint8_t *recs;      /* its records: colour << 24 | span block */
    const uint8_t *dir;       /* u32 BE record indices */
    uint32_t *out;
    int n;
    int cur;                  /* record cursor (the original's si) */
    /* the current tile */
    int dircell;              /* index of the directory cell's first kind */
    uint16_t tile, nearer, inner;
    int half;
} comp;

static void seek_kind(comp *k, int kind)
{
    k->cur = (int)get32(k->dir + 4 * (k->dircell + kind));
}

/* 0x3137 / 0x3174: one record, colour patched if override != 0. */
static void fill(comp *k, int override)
{
    uint32_t rec = get32(k->recs + 4 * k->cur++);
    int col = override ? override : (int)(rec >> 24);
    const uint8_t *q = k->c->tables + TAB_QUAD + 4 * (col < 74 ? col : 0);
    uint32_t fillb = q[k->half];
    k->out[k->n++] = k->c->trekg_at + (rec & 0xFFFFFFu);
    k->out[k->n++] = fillb | ((uint32_t)k->half << 8);
}

static void skip(comp *k)      /* fn_31b5 */
{
    k->cur++;
}

static int shape(uint16_t t) { return (t >> 8) & 0xf; }

static int blockcolor(uint16_t t)
{
    int c = (t >> 4) & 0xf;
    return c ? c : 0x3d;
}

static void compose_floor(comp *k)              /* fn_2e50 */
{
    int c = k->tile & 0xf;
    if (c == 0)
        return;
    seek_kind(k, 0);
    fill(k, c);                                         /* top */
    if ((k->inner & 0xf) == 0)
        fill(k, c + 0x1e);                              /* side edge */
    else
        skip(k);
    if ((k->nearer & 0xf) == 0)
        fill(k, c + 0xf);                               /* front edge */
}

static void compose_lowblock(comp *k)           /* 0x2e9f */
{
    compose_floor(k);
    if (shape(k->nearer) < 2) {
        seek_kind(k, 3);
        fill(k, 0);
    }
    seek_kind(k, 2);
    fill(k, blockcolor(k->tile));
    if (shape(k->inner) < 2)
        fill(k, 0);
}

static void compose_tun_low(comp *k)            /* 0x2ee1 */
{
    compose_floor(k);
    if (shape(k->nearer) < 2) {
        seek_kind(k, 1);
        fill(k, 0x41);
    }
    seek_kind(k, 2);
    fill(k, blockcolor(k->tile));
    if (shape(k->inner) < 2)
        fill(k, 0);
    if (shape(k->nearer) < 2) {
        seek_kind(k, 3);
        skip(k);
        fill(k, 0);
        fill(k, 0);
    }
}

static void compose_highblock(comp *k)          /* 0x2f3c */
{
    compose_floor(k);
    if (shape(k->nearer) < 2) {
        seek_kind(k, 3);
        fill(k, 0);
    }
    seek_kind(k, 2);
    skip(k);
    if (shape(k->inner) < 2)
        fill(k, 0);
    seek_kind(k, 5);
    fill(k, blockcolor(k->tile));
    if (shape(k->inner) < 4)
        fill(k, 0);
    else
        skip(k);
    if (shape(k->nearer) < 4)
        fill(k, 0);
}

static void compose_tunnel(comp *k)             /* 0x303d */
{
    compose_floor(k);
    if (shape(k->nearer) < 1) {
        seek_kind(k, 1);
        fill(k, 0x43);
    }
    seek_kind(k, 4);
    for (int i = 0; i < 6; i++)
        fill(k, 0);
    if (shape(k->nearer) < 1) {
        fill(k, 0);
        fill(k, 0);
    }
}

/* 0x2fb0.  skyroads-port's render.c draws here kind 4 - the tunnel's arch
 * gradient, six records - which the EXE does not: after the lower side it
 * draws kind 3's split tops (skip one, then two), when the nearer tile is
 * low, exactly as compose_tun_low ends (disassembly 0x2fe6-0x2ff7).  The
 * port painted the inside of the bore over the block (roads 8 and 9, seen
 * on the Jaguar by the user against the original, session 3). */
static void compose_tun_high(comp *k)
{
    compose_floor(k);
    if (shape(k->nearer) < 2) {
        seek_kind(k, 1);
        fill(k, 0x41);
    }
    seek_kind(k, 2);
    skip(k);
    if (shape(k->inner) < 2)
        fill(k, 0);
    if (shape(k->nearer) < 2) {
        seek_kind(k, 3);
        skip(k);
        fill(k, 0);
        fill(k, 0);
    }
    seek_kind(k, 5);
    fill(k, blockcolor(k->tile));
    if (shape(k->inner) < 4)
        fill(k, 0);
    else
        skip(k);
    if (shape(k->nearer) < 4)
        fill(k, 0);
}

static void compose_tile(comp *k)
{
    switch (shape(k->tile)) {
    case 0: compose_floor(k); break;
    case 1: compose_tunnel(k); break;
    case 2: compose_lowblock(k); break;
    case 3: compose_tun_low(k); break;
    case 4: compose_highblock(k); break;
    case 5: compose_tun_high(k); break;
    default: break;
    }
}

static void emit(comp *k, uint32_t a, uint32_t b)
{
    k->out[k->n++] = a;
    k->out[k->n++] = b;
}

/* One grid row's pass over both halves, directory row dirrow. */
static void row_pass(comp *k, const sr_play *p, int grow, int dirrow)
{
    for (int half = 0; half < 2; half++)
        for (int ci = 0; ci < 4; ci++) {
            int gcol = half == 0 ? ci : 6 - ci;
            uint16_t tile = 0, nearer = 0, inner = 0;
            if (grow >= 0 && grow < SR_GRID_ROWS) {
                const uint16_t *row = p->road->cells + grow * 7;
                tile = row[gcol];
                int in_col = half == 0 ? gcol + 1 : gcol - 1;
                if (in_col >= 0 && in_col < 7)
                    inner = row[in_col];
                if (grow - 1 >= 0)
                    nearer = p->road->cells[(grow - 1) * 7 + gcol];
            }
            k->dircell = dirrow * 24 + ci * 6;
            k->tile = tile;
            k->nearer = nearer;
            k->inner = inner;
            k->half = half;
            compose_tile(k);
        }
}

void road_compose(road_ctx *c, const sr_play *p)
{
    comp k;
    uint16_t pos = (uint16_t)(p->z / 0x2000);
    uint32_t road_base = c->view + ROAD_Y0 * 320;
    k.c = c;
    k.obj = c->trekg + get32(c->trekg + 4 * (pos & 7));
    k.recs = k.obj + get32(k.obj + 4);
    k.dir = k.obj + 8;
    k.out = c->cmd;
    k.n = 0;
    k.cur = 0;

    /* First launch: the records drawn before the ship.  The pristine
     * screen (fn_3492's restore) is already there: the Blitter copies it on
     * the Jaguar, memcpy on the PC (road.h, "THE FRAME"). */
    emit(&k, ROAD_OP_BASE, road_base);

    int baserow = pos >> 3;
    for (int dr = 0; dr < 10; dr++) {
        int grow = baserow + 7 - dr;
        if (dr == 7) {
            row_pass(&k, p, grow, 11);              /* pre-ship (0x210) */
            emit(&k, ROAD_OP_END, 0);               /* the ship goes here */
            c->second = k.n;
            emit(&k, ROAD_OP_BASE, road_base);
            row_pass(&k, p, grow, 12);              /* post-ship (0x240) */
        } else {
            row_pass(&k, p, grow, dr);
        }
    }
    emit(&k, ROAD_OP_END, 0);
}

/* ---- the GPU's input (road.h, CIN_*) ------------------------------------ */

#if defined(__m68k__)
static inline void st32(uint8_t *b, uint32_t v) { *(uint32_t *)(void *)b = v; }
#else
static inline void st32(uint8_t *b, uint32_t v) { put32(b, v); }
#endif

void road_gpu_quads(uint8_t *cin, const uint8_t *tables)
{
    for (int i = 0; i < 74; i++) {
        st32(cin + CIN_QUAD0 + 4 * i, tables[TAB_QUAD + 4 * i]);
        st32(cin + CIN_QUAD1 + 4 * i, tables[TAB_QUAD + 4 * i + 1]);
    }
}

void road_gpu_input(uint8_t *cin, const sr_play *p, uint32_t view)
{
    uint16_t pos = (uint16_t)(p->z / 0x2000);
    st32(cin + CIN_POS, pos);
    st32(cin + CIN_BASE, view + ROAD_Y0 * 320);
    int g = (pos >> 3) - 3;
    uint8_t *w = cin + CIN_WIN;
    for (int r = 0; r < CIN_ROWS; r++, g++) {
        int in = g >= 0 && g < SR_GRID_ROWS;
        for (int i = 0; i < 7; i++, w += 4)
            st32(w, in ? p->road->cells[g * 7 + i] : 0);
        st32(w, 0);
        w += 4;
    }
}

/* ---- the shadow (render.c draw_shadow / support, fn_0b71) --------------- */

static uint16_t support(const sr_play *p, uint32_t z, uint16_t x, int in_tun)
{
    uint16_t t = sr_tile_at(p, z, x);
    int sh = (t >> 8) & 0xf;
    if (sh >= 2 && sh <= 5)
        return sr_blocktop[sh];
    if (sh == 1 && !in_tun)
        return 0;
    return (t & 0xf) ? 0x2800 : 0;
}

/* The dashboard cowl over the viewport (render.c cowl_hidden): rows above
 * 137 are always hidden; on the others, |x - 160| < half-width hides. */
static int cowl_row(const road_ctx *c, int y, int *lo, int *hi)
{
    if (y < 0 || y > 137)
        return 0;
    int hw = get16(c->tables + TAB_COWL + 2 * y);
    *lo = hw ? 160 - hw : 1;              /* hidden: lo < x < hi */
    *hi = hw ? 160 + hw : 0;
    return 1;
}

/* render.c draw_ship: 29 screen columns x 24 rows, index 0 transparent,
 * clipped by the dashboard cowl exactly as the original.  Pointer rows, no
 * per-pixel multiply: the 68000 has no 32-bit one, and this loop paid a
 * library call per pixel (M3, measured on hardware). */
void road_ship(const road_ctx *c, uint8_t *view, const uint8_t *cars, ship_pose_t s)
{
    if (s.frame < 0)
        return;
    const uint8_t *src = cars + CAR_FRAME_BYTES * s.frame;
    uint8_t *row = view + (int32_t)(int16_t)s.top * 320;
    for (int i = 0; i < 24; i++, src += 32, row += 320) {
        int lo, hi;
        if (!cowl_row(c, s.top + i, &lo, &hi))
            continue;
        for (int j = 0; j < 29; j++) {
            uint8_t px = src[j];
            int x = s.left + j;
            if (!px || x < 0 || x >= 320 || (x > lo && x < hi))
                continue;
            row[x] = px;
        }
    }
}

int road_shadow_where(const sr_play *p, ship_pose_t s, int *map, int *top, int *left)
{
    if (s.frame < 0 || p->expl_ctr)
        return 0;                         /* render.c: no shadow while exploding */
    int in_tun = sr_in_tunnel(p, p->z, p->x, p->y);
    uint16_t g1 = support(p, p->z, (uint16_t)(p->x - 0x380), in_tun);
    uint16_t g2 = support(p, p->z, (uint16_t)(p->x + 0x380), in_tun);
    uint16_t g = g1 > g2 ? g1 : g2;
    if (!g)
        return 0;
    int clearance = ((int)p->y - (int)g) / 0x80;
    if (clearance < 0)
        return 0;
    *map = clearance / 5;
    if (*map >= 5)
        return 0;
    *left = s.left;
    *top = s.top + 0x10 + clearance;
    return 1;
}

void road_shadow_draw(const road_ctx *c, uint8_t *view, int map, int top, int left)
{
    const uint8_t *stencil = c->tables + TAB_SHADOW + 261 * map;
    /* The stencils are ROW-major, 9 rows of 29: read that way each one is the
     * ship's silhouette seen from above, engine pods and all.  skyroads-port
     * (render.c, renderer.md "29x9 column-major") reads them column by column,
     * which turns the shadow into separate vertical bars - seen on the real
     * Jaguar by the user (M2), while our PC check, being a comparison WITH the
     * port, could only agree with it. */
    uint8_t *row = view + (int32_t)(int16_t)top * 320;
    for (int i = 0; i < 9; i++, stencil += 29, row += 320) {
        int lo, hi;
        if (!cowl_row(c, top + i, &lo, &hi))
            continue;
        for (int j = 0; j < 29; j++) {
            int x = left + j;
            if (!stencil[j] || x < 0 || x >= 320 || (x > lo && x < hi))
                continue;
            uint8_t *q = row + x;
            if (*q >= 1 && *q <= 15)
                *q = (uint8_t)(*q + 0x2d);
            else if (*q == 0x3d)
                *q = 0x40;
        }
    }
}

void road_shadow(const road_ctx *c, uint8_t *view, const sr_play *p, ship_pose_t s)
{
    int map, top, left;
    if (road_shadow_where(p, s, &map, &top, &left))
        road_shadow_draw(c, view, map, top, left);
}

/* ---- the ship and shadow for the GPU (road.h, SIN_*) -------------------- */

void road_shadow_masks(uint8_t *dst, const uint8_t *tables)
{
    for (int m = 0; m < 5; m++)
        for (int i = 0; i < 9; i++) {
            const uint8_t *st = tables + TAB_SHADOW + 261 * m + 29 * i;
            uint32_t bits = 0;
            for (int j = 0; j < 29; j++)
                if (st[j])
                    bits |= 0x80000000u >> j;
            st32(dst + 4 * (9 * m + i), bits);
        }
}

/* One row's clip long: see SIN_CLIP. */
static uint32_t clip_long(const road_ctx *c, int y)
{
    int lo, hi;
    if (!cowl_row(c, y, &lo, &hi))
        return 320u;                      /* A = -1, B = 320: nothing */
    if (lo < -1) lo = -1;
    if (hi > 320) hi = 320;
    return (uint32_t)(lo + 1) << 16 | (uint32_t)hi;
}

void road_ship_input(uint8_t *sin, const road_ctx *c, const sr_play *p, ship_pose_t s,
                     uint32_t cars_at, uint32_t masks_at)
{
    int map = 0, top = 0, left = 0;
    int shadow = road_shadow_where(p, s, &map, &top, &left);
    road_ship_input_at(sin, c, s, shadow, map, top, left, cars_at, masks_at);
}

void road_ship_input_at(uint8_t *sin, const road_ctx *c, ship_pose_t s,
                        int shadow, int map, int top, int left,
                        uint32_t cars_at, uint32_t masks_at)
{
    if (s.frame < 0) {
        st32(sin + SIN_SRC, 0);
    } else {
        st32(sin + SIN_SRC, cars_at + CAR_FRAME_BYTES * (uint32_t)s.frame);
        st32(sin + SIN_ROW0, c->view + (uint32_t)((int32_t)s.top * 320));
        st32(sin + SIN_LEFT, (uint32_t)(int32_t)s.left);
        for (int i = 0; i < 24; i++)
            st32(sin + SIN_CLIP + 4 * i, clip_long(c, s.top + i));
    }
    if (!shadow) {
        st32(sin + SIN_MASK, 0);
        return;
    }
    st32(sin + SIN_MASK, masks_at + 4 * 9 * (uint32_t)map);
    st32(sin + SIN_SROW0, c->view + (uint32_t)((int32_t)top * 320));
    st32(sin + SIN_SLEFT, (uint32_t)(int32_t)left);
    for (int i = 0; i < 9; i++)
        st32(sin + SIN_SCLIP + 4 * i, clip_long(c, top + i));
}
