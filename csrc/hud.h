/* The dashboard's moving parts and the pristine screen.
 *
 * Compiled into the Jaguar blob AND into the PC tools.  The drawing is
 * skyroads-port's hud.c (renderer.md sec.9; MIT, port/LICENSE-skyroads-port.txt)
 * over hud.bin (tools/sr2jag.py): stateless, over a frame that starts every
 * time from the pristine screen, as the original's does.
 */
#ifndef HUD_H
#define HUD_H

#include "road.h"

#define HUD_N_SPEED 34
#define HUD_N_OXY   10
#define HUD_N_FUL   10

/* The pristine 320x200 screen of a road: the world backdrop in rows 0..137,
 * the dashboard picture from row 129 - transparent where it overlaps the
 * backdrop (rows 129..137), its zeros kept below (palette entry 0).  It is
 * render.c's sr_render_set_world. */
void hud_pristine(uint8_t *pristine, const uint8_t *world, const uint8_t *dash);

/* The gauges, over a frame that already holds the pristine dashboard: the
 * stateless redraw of skyroads-port's hud.c. */
void hud_draw(uint8_t *view, const uint8_t *pristine, const uint8_t *hud,
              const sr_play *p);

/* The same picture, kept up to date incrementally in a frame buffer whose
 * rows HUD_ROW0..199 persist between frames (only the viewport is restored
 * every frame).  st describes what this buffer shows; valid = 0 (a new road,
 * a new buffer) redraws everything.  Must equal hud_draw pixel for pixel:
 * tools/oracle checks it.  All gauges live at y >= 138 (tools/sr2jag.py
 * asserts the segments, the rest are fixed positions). */
#define HUD_ROW0 138
typedef struct {
    int16_t valid, speed, oxy, ful, steps, lamp;
} hud_state;
void hud_update(uint8_t *view, const uint8_t *pristine, const uint8_t *hud,
                const sr_play *p, hud_state *st);

/* An instrument, not part of the game: v in n decimal digits, the
 * GRAV-O-METER's digit shapes scaled up, colour fg on a bg box. */
void hud_number(uint8_t *view, const uint8_t *hud, int x, int y, uint32_t v, int n,
                int scale, uint8_t fg, uint8_t bg);

#endif
