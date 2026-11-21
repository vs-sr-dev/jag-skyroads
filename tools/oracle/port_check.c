/* DEV TOOL, not part of the build: our whole frame path (csrc/road.c,
 * csrc/hud.c, tools/oracle/raster.h, the cart/ blobs) against
 * skyroads-port's own renderer + HUD, pixel by pixel - every tick of the
 * attract demo, then synthetic sweeps along all 31 roads.
 *
 * Needs the skyroads-port checkout in ref/ (not in this repository) and the
 * game data unzipped in gamedata/.  The port's render.c reads the shadow
 * stencils column-major, which is wrong (docs/PLAN.md); patch it first:
 *
 *   C=ref/skyroads-port-main/src/core
 *   sed 's/if (!stencil\[j \* 9 + i\])/if (!stencil[i * 29 + j])/' $C/render.c > build/render_fixed.c
 *   gcc -std=c11 -O2 -w -I$C -Itools/oracle -Icsrc -o build/port_check.exe \
 *       tools/oracle/port_check.c build/render_fixed.c $C/hud.c $C/assets.c \
 *       $C/lzs.c $C/tables.c $C/gfx.c csrc/port/play.c csrc/road.c csrc/hud.c
 *   build/port_check.exe gamedata cart [tick-to-dump]
 *
 * Expected: "0 frames differ" twice.
 */
/* Dev-only: our road path (csrc/road.c + tools/oracle/raster.h + the cart
 * blobs) against skyroads-port's sr_render_frame, every tick of the demo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "render.h"
#include "hud.h"
#include "raster.h"

static const char *g_dir;
static void *io_read_file(const char *name, size_t *out_size)
{
    char path[1200], upper[64];
    size_t n = strlen(name);
    for (size_t i = 0; i <= n; i++) {
        char c = name[i];
        upper[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        snprintf(path, sizeof path, "%s/%s", g_dir, attempt ? upper : name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *buf = malloc((size_t)size);
        fread(buf, 1, (size_t)size, f);
        fclose(f);
        *out_size = (size_t)size;
        return buf;
    }
    return NULL;
}

static uint8_t *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("no %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); fread(b, 1, n, f); fclose(f); return b;
}

static void dump(const char *name, const uint8_t *fb)
{
    FILE *f = fopen(name, "wb"); fwrite(fb, 1, 320 * 200, f); fclose(f);
}

int main(int argc, char **argv)
{
    g_dir = argv[1];
    const char *cart = argv[2];
    char p[1024];
    static sr_assets a;
    char err[256];
    if (!sr_assets_load(&a, (sr_io){ io_read_file, NULL }, err, sizeof err)) { printf("%s\n", err); return 1; }
    sr_road road;
    sr_assets_load_road(&a, 0, &road);
    sr_assets_load_world(&a, 0);
    static sr_render rend;
    sr_render_init(&rend, &a);
    sr_render_set_world(&rend, &a);

    snprintf(p, sizeof p, "%s/trekg.bin", cart); uint8_t *trekg = slurp(p);
    snprintf(p, sizeof p, "%s/tables.bin", cart); uint8_t *tables = slurp(p);
    snprintf(p, sizeof p, "%s/worlds.bin", cart); uint8_t *worlds = slurp(p);
    snprintf(p, sizeof p, "%s/dash.bin", cart); uint8_t *dash = slurp(p);
    snprintf(p, sizeof p, "%s/cars.bin", cart); uint8_t *cars = slurp(p);
    snprintf(p, sizeof p, "%s/hud.bin", cart); uint8_t *hudb = slurp(p);
    static uint8_t pristine[VIEW_BYTES];
    hud_pristine(pristine, worlds, dash);

    static sr_play play;
    sr_play_init(&play, &road, a.demo, a.demo_size);
    static uint32_t cmd[ROAD_CMD_MAX];
    static uint8_t views[2][VIEW_BYTES]; uint8_t *view = views[0], *ours = view; static hud_state hst[2]; unsigned kf = 0;
    static sr_fb theirs;
    road_ctx c = { trekg, HOST_TREKG, tables, HOST_VIEW, HOST_PRISTINE, cmd, 0 };

    int res = SR_RES_RUNNING, t, bad_frames = 0, worst = 0, maxcmd = 0;
    long bad_px = 0, bad_ship = 0;
    for (t = 0; t < 36 * 240 && res == SR_RES_RUNNING; t++) {
        sr_input none = { 0 };
        sr_play_input(&play, &none);
        res = sr_play_tick(&play);
        sr_render_frame(&rend, &theirs, &a, &play, play.tick, play.on_sticky);
        sr_hud_draw(&theirs, &a, rend.pristine, &play, play.tick);

        ship_pose_t s = ship_pose(&play, play.tick, play.on_sticky);
        kf++; view = ours = views[kf & 1]; render_view(&c, &play, view, pristine, cars, hudb, s, &hst[kf & 1]);
        int n = 0;
        while (!(cmd[c.second + n] == 0 && n > 0 && cmd[c.second + n - 2] != 0) && n < ROAD_CMD_MAX) n += 2;
        if (c.second + n > maxcmd) maxcmd = c.second + n;
        if (argc > 3 && t == atoi(argv[3])) { dump("frame_ours.raw", ours); dump("frame_theirs.raw", theirs.px); }

        int d = 0, dship = 0;
        for (int i = 0; i < 320 * 200; i++)
            if (ours[i] != theirs.px[i]) {
                d++;
                int x = i % 320, y = i / 320;
                if (x >= s.left && x < s.left + 32 && y >= s.top && y < s.top + 24) dship++;
            }
        if (d) {
            bad_frames++; bad_px += d; bad_ship += dship;
            if (d > worst) {
                worst = d;
                dump("worst_ours.raw", ours);
                dump("worst_theirs.raw", theirs.px);
                printf("tick %d: %d px differ (%d inside the ship box)\n", t, d, dship);
            }
        }
    }
    printf("%d ticks, %d frames differ, %ld px total (%ld in ship box), max cmd longs %d of %d\n",
           t, bad_frames, bad_px, bad_ship, maxcmd, ROAD_CMD_MAX);
    /* every road, synthetic sweep */
    snprintf(p, sizeof p, "%s/roads.bin", cart); uint8_t *roadsb = slurp(p);
    long tot_frames = 0, tot_bad = 0;
    for (int e = 0; e < 31; e++) {
        sr_road r2; static uint16_t grid[SR_GRID_ROWS * 7]; sr_road ours_road;
        sr_assets_load_road(&a, e, &r2);
        int w = e == 0 ? 0 : (e - 1) / 3;
        sr_assets_load_world(&a, w);
        sr_render_set_world(&rend, &a);
        road_load(roadsb, e, &ours_road, grid);
        hud_pristine(pristine, worlds + w * 44160, dash);
        hst[0].valid = hst[1].valid = 0;
        static sr_play pt, po;
        sr_play_init(&pt, &r2, NULL, 0);
        sr_play_init(&po, &ours_road, NULL, 0);
        int bad = 0, nf = 0;
        for (uint32_t z = 3u << 16; z < ((uint32_t)r2.rows << 16); z += 0x2000 * 3) {
            for (int xi = 0; xi < 3; xi++) {
                uint16_t x = (uint16_t)(0x4000 + xi * 0x4000);
                pt.z = po.z = z; pt.x = po.x = x; pt.y = po.y = 0x2800 + 0x400 * xi;
                pt.tick = po.tick = z >> 13;
                sr_render_frame(&rend, &theirs, &a, &pt, pt.tick, 0);
                sr_hud_draw(&theirs, &a, rend.pristine, &pt, pt.tick);
                ship_pose_t sp = ship_pose(&po, po.tick, 0);
                kf++; view = views[kf & 1]; render_view(&c, &po, view, pristine, cars, hudb, sp, &hst[kf & 1]);
                int d = 0;
                for (int i = 0; i < 320 * 200; i++) d += view[i] != theirs.px[i];
                if (d) { if (!bad) printf("road %d z=%08x x=%04x: %d px differ\n", e, z, x, d); bad++; }
                nf++;
            }
        }
        tot_frames += nf; tot_bad += bad;
    }
    printf("all roads: %ld frames, %ld differ\n", tot_frames, tot_bad);
    return 0;
}
