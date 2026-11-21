/* The road's composers: see road.h.  Function names and EXE addresses follow
 * skyroads-port's render.c, which this file transcribes into commands. */
#include "road.h"

typedef struct {
    const road_ctx *c;
    const uint8_t *obj;       /* phase object in trekg */
    const uint8_t *col;       /* its record colours */
    const uint8_t *offs;      /* its span-block offsets, u32 BE */
    const uint8_t *dir;       /* u16 BE record indices */
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
    k->cur = get16(k->dir + 2 * (k->dircell + kind));
}

/* 0x3137 / 0x3174: one record, colour patched if override != 0. */
static void fill(comp *k, int override)
{
    int r = k->cur++;
    int col = override ? override : k->col[r];
    const uint8_t *q = k->c->tables + TAB_QUAD + 4 * (col < 74 ? col : 0);
    uint32_t fillb = q[k->half];
    k->out[k->n++] = k->c->trekg_at + get32(k->offs + 4 * r);
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

static void compose_tun_high(comp *k)           /* 0x2fb0 */
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
    seek_kind(k, 4);
    for (int i = 0; i < 6; i++)
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
    k.col = k.obj + get32(k.obj + 4);
    k.offs = k.obj + get32(k.obj + 8);
    k.dir = k.obj + 12;
    k.out = c->cmd;
    k.n = 0;
    k.cur = 0;

    /* First launch: the pristine screen (fn_3492's restore), then the
     * records drawn before the ship. */
    emit(&k, ROAD_OP_BASE, c->view);
    emit(&k, ROAD_OP_COPY, c->backdrop);
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

static int cowl_hidden(const road_ctx *c, int x, int y)
{
    if (y < 0 || y > 137)
        return 1;
    uint16_t hw = get16(c->tables + TAB_COWL + 2 * y);
    if (!hw)
        return 0;
    return x > 160 - (int)hw && x < 160 + (int)hw;
}

/* render.c draw_ship: 29 screen columns x 24 rows, index 0 transparent,
 * clipped by the dashboard cowl exactly as the original. */
void road_ship(const road_ctx *c, uint8_t *view, const uint8_t *cars, ship_pose_t s)
{
    if (s.frame < 0)
        return;
    const uint8_t *cell = cars + CAR_FRAME_BYTES * s.frame;
    for (int i = 0; i < 24; i++)
        for (int j = 0; j < 29; j++) {
            uint8_t px = cell[i * 32 + j];
            if (!px)
                continue;
            int x = s.left + j, y = s.top + i;
            if (x < 0 || x >= 320 || cowl_hidden(c, x, y))
                continue;
            view[y * 320 + x] = px;
        }
}

void road_shadow(const road_ctx *c, uint8_t *view, const sr_play *p, ship_pose_t s)
{
    if (s.frame < 0 || p->expl_ctr)
        return;                           /* render.c: no shadow while exploding */
    int in_tun = sr_in_tunnel(p, p->z, p->x, p->y);
    uint16_t g1 = support(p, p->z, (uint16_t)(p->x - 0x380), in_tun);
    uint16_t g2 = support(p, p->z, (uint16_t)(p->x + 0x380), in_tun);
    uint16_t g = g1 > g2 ? g1 : g2;
    if (!g)
        return;
    int clearance = ((int)p->y - (int)g) / 0x80;
    if (clearance < 0)
        return;
    int map = clearance / 5;
    if (map >= 5)
        return;
    const uint8_t *stencil = c->tables + TAB_SHADOW + 261 * map;
    int left = s.left, top = s.top + 0x10 + clearance;
    /* The stencils are ROW-major, 9 rows of 29: read that way each one is the
     * ship's silhouette seen from above, engine pods and all.  skyroads-port
     * (render.c, renderer.md "29x9 column-major") reads them column by column,
     * which turns the shadow into separate vertical bars - seen on the real
     * Jaguar by the user (M2), while our PC check, being a comparison WITH the
     * port, could only agree with it. */
    for (int j = 0; j < 29; j++)
        for (int i = 0; i < 9; i++) {
            if (!stencil[i * 29 + j])
                continue;
            int x = left + j, y = top + i;
            if (x < 0 || x >= 320 || cowl_hidden(c, x, y))
                continue;
            uint8_t *q = view + y * 320 + x;
            if (*q >= 1 && *q <= 15)
                *q = (uint8_t)(*q + 0x2d);
            else if (*q == 0x3d)
                *q = 0x40;
        }
}
