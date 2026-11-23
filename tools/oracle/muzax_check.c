/* skyroads-port's MUZAX driver (src/core/audio.c), its register writes dumped
 * tick by tick, for tools/oracle/muzax_check.py to compare with tools/muzax.py.
 *
 *   muzax_check MUZAX.LZS TICKS > out.txt
 *
 * audio.c is compiled INTO this file (its music_tick is static), against the
 * reference as frozen in ref/; the OPL3 emulator is replaced by a recorder.
 * Output:
 *   I w w ...           the boot init's writes (opl_init)
 *   S n                 song n:
 *   R w w ...             music_start's writes (the reset)
 *   T k w w ...           tick k's writes (music_tick)
 * w = reg << 8 | value, in hex.  A dev tool: never linked into the ROM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../ref/skyroads-port-main/src/core/audio.c"

static unsigned rec[4096];
static int nrec;

void OPL3_Reset(opl3_chip *chip, uint32_t samplerate) { (void)chip; (void)samplerate; }
void OPL3_GenerateResampled(opl3_chip *chip, int16_t *buf) { (void)chip; buf[0] = buf[1] = 0; }
void OPL3_WriteRegBuffered(opl3_chip *chip, uint16_t reg, uint8_t v)
{
    (void)chip;
    if (nrec < 4096)
        rec[nrec++] = (unsigned)((reg & 0xff) << 8 | v);
}

static void flush(const char *tag, int k)
{
    if (k >= 0)
        printf("%s %d", tag, k);
    else
        printf("%s", tag);
    for (int i = 0; i < nrec; i++)
        printf(" %04x", rec[i]);
    printf("\n");
    nrec = 0;
}

static const char *g_path;
static void *read_file(const char *name, size_t *size)
{
    (void)name;
    FILE *f = fopen(g_path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: muzax_check MUZAX.LZS TICKS\n");
        return 2;
    }
    g_path = argv[1];
    int ticks = atoi(argv[2]);
    sr_assets assets;
    memset(&assets, 0, sizeof assets);
    assets.io.read_file = read_file;

    sr_audio *a = sr_audio_create();
    flush("I", -1);
    for (int n = 0; n < 14; n++) {
        printf("S %d\n", n);
        a->cur_song = -1;
        if (!sr_audio_music(a, &assets, n)) {
            fprintf(stderr, "song %d did not load\n", n);
            return 1;
        }
        flush("R", -1);
        for (int k = 0; k < ticks; k++) {
            /* THE PORT IS ONE TICK SLOW ON EVERY DELAY.  The EXE's delay
             * handler (0x58f8) sets the counter and returns into the fetch
             * loop, which jumps back to the test (0x5a5f: jmp 0x5a3c) and
             * takes one off in the same tick: a delay of N resumes N ticks
             * later.  audio.c returns at once, so it resumes after N + 1.
             * Corrected here so the rest can be compared. */
            int was = a->delay;
            music_tick(a);
            if (was == 0 && a->delay)
                a->delay--;
            flush("T", k);
        }
    }
    sr_audio_destroy(a);
    return 0;
}
