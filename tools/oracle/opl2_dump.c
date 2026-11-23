/* The OPL2 model's samples for one scenario, for tools/oracle/dsp_check.py
 * to compare with the DSP binary run in the JRISC interpreter.
 *
 *   opl2_dump muzax.bin sfx.bin N SONG AT_SONG SFX AT_SFX out.bin [SKIP]
 *
 * SKIP: bytes into the song's stream to start from (dsp_check skips the
 * original's 1.4 s wait with it).
 * Before sample 0 the INIT stream is posted (as InitDspAudio does), before
 * sample AT_SONG stream SONG (1 + song number; 15 = silence), before sample
 * AT_SFX effect SFX (-1: none).  N samples, int16 little-endian.
 */
#include <stdio.h>
#include <stdlib.h>
#include "opl2_model.h"

static uint8_t *load(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    return b;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char **argv)
{
    if (argc < 9) { fprintf(stderr, "usage: opl2_dump muzax.bin sfx.bin N SONG AT_SONG SFX AT_SFX out.bin\n"); return 2; }
    uint8_t *mz = load(argv[1]), *sx = load(argv[2]);
    long n = atol(argv[3]), at_song = atol(argv[5]), at_sfx = atol(argv[7]);
    int song = atoi(argv[4]), sfx = atoi(argv[6]);
    static opl2 c;
    opl2_init(&c);
    opl2_song(&c, mz + be32(mz + 4), mz + be32(mz + 8));
    FILE *f = fopen(argv[8], "wb");
    for (long i = 0; i < n; i++) {
        if (i == at_song)
            opl2_song(&c, mz + be32(mz + 4 + 8 * song) + (argc > 9 ? atol(argv[9]) : 0),
                      mz + be32(mz + 8 + 8 * song));
        if (i == at_sfx && sfx >= 0)
            opl2_sfx(&c, (const int8_t *)sx + be32(sx + 4 + 8 * sfx), be32(sx + 8 + 8 * sfx));
        if (getenv("DUMP_AT") && i == atol(getenv("DUMP_AT"))) {
            for (int k = 0; k < 18; k++) {
                opl2_slot *q = &c.s[k];
                fprintf(stderr, "%d ATT3=%X PH=%X INC=%X OUT=%X PROUT=%X ENV=%X EGST=%X EGINC=%X EGLIM=%X ATTS=%X KEY=%X\n",
                        k, q->att3, q->ph, q->inc, (unsigned)q->out, (unsigned)q->prout, q->env, q->egst, q->eginc, q->eglim, q->atts, q->key);
            }
            fprintf(stderr, "trem=%X vibpos=%X tpos=%X vcnt=%X rhy=%X\n", c.trem, c.vibpos, c.tpos, c.vcnt, c.rhy);
        }
        int16_t v = (int16_t)opl2_sample(&c);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return 0;
}
