/* JagSkyroads - the front end: intro, menus, road map (csrc/front.c).
 *
 * A transcription of SKYROADS.EXE's screens, not of skyroads-port's (whose
 * game.c approximates them): fn_4575 intro, fn_4e36 main menu, fn_4c04
 * settings, fn_4e12/fn_4dac help, fn_5164 road map, the text of fn_2b21.
 * The original draws straight into VGA memory and blocks while it waits;
 * so does this, into one frame buffer the OP shows (FRONT_VIEW), consuming
 * game ticks as it waits (fe_tick).
 */
#ifndef FRONT_H
#define FRONT_H

#include <stdint.h>

/* front.bin's pictures and colour maps, in tools/sr2jag.py front_blob's
 * order (the blob carries the counts; fe_init checks them). */
enum {
    P_TITLE, P_LOGO, P_SCENE0,
    P_BOX0 = P_SCENE0 + 7,
    P_SET_BG = P_BOX0 + 3, P_SET_ITEM0,
    P_HELP0 = P_SET_ITEM0 + 10,
    P_GO_BG = P_HELP0 + 2, P_GO_TICK,
    P_ANIM0
};
enum {
    C_A, C_B, C_C, C_E0,
    C_G0 = C_E0 + 7,
    C_MENU = C_G0 + 7, C_SET1, C_SET2, C_HELP0,
    C_GO = C_HELP0 + 2, C_GO2, C_ANIM,
    C_COUNT
};

/* skyroads.cfg (EXE 0x571b / 0x5770): the control device (0 keyboard,
 * 1 joystick, 2 mouse: shown, meaningless on a Jaguar pad), sound off, and
 * how many times each road was completed. */
typedef struct {
    uint16_t control;
    uint16_t sound_off;
    uint16_t done[30];
} sr_cfg;

extern sr_cfg cfg;
extern int fe_road;            /* the road map's selection, 0..29 (ds:0x933e) */

int  fe_init(void);            /* c_init: black DAC, FRONT_VIEW on screen; 0 = bad blob */
void fe_show(void);            /* put FRONT_VIEW on screen (from the game) */
void fe_flush(void);           /* forget the pad's history (leaving a road) */
void fe_cfg_load(void);        /* fn_571b, from the cartridge EEPROM */
void fe_cfg_save(void);        /* fn_5770 */

int  fe_intro(void);           /* 1 if a key cut it short */
void fe_mainmenu(int fade_in); /* 0 right after an interrupted intro */
int  fe_gomenu(void);          /* 1 = ESC (back to the main menu) */

/* play_road's frame (fn_2b21): fn_4b72 on a 256-colour palette, the text. */
void fe_fade_full(const uint8_t *pal, int in, int steps);
void fe_wait(int ticks);
void fe_text(uint8_t *screen, int x, int y, const char *s, uint8_t colour);
void fe_wait_shown(void);      /* until the OP shows ROAD_SEQ's frame */

/* game_main.c's, for the front end */
void music(int n);
void music_stop(void);
void sfx(int n);               /* an effect, unless the sound is off */
void sfx_voice(void);          /* INTRO.SND: played whatever the setting (0x4775) */
void audio_post(void);

#endif
