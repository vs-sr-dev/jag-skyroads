/* One song through the OPL2 model (tools/oracle/opl2_model.c, the DSP's
 * specification) and through Nuked OPL3, side by side.
 *
 *   opl2_render muzax.bin SONG SECONDS ours.wav nuked.wav
 *
 * Both play the same stream (tools/muzax.py): INIT on the first tick, then
 * song SONG.  The model runs at Jerry's Fs; Nuked at the YM3812's 49,716 Hz,
 * with its buffered writes (2 chip samples apart, like the card's ~27 us
 * per write).  The rates differ, so the samples cannot be compared; what is
 * compared is what the ear follows:
 *   - the loudness, in 20 ms windows (dB), where the reference is audible;
 *   - the spectrum below Fs/2, in 4096-point frames (log-spectral distance).
 * Printed as one line; the WAVs are for listening.  A dev tool, never linked
 * into the ROM (Nuked OPL3 is LGPL, read from ref/).
 */
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opl2_model.h"
#include "../../build/opl2_tab.h"
#include "../../ref/skyroads-port-main/src/thirdparty/opl3.h"

#define NUKED_FS 49716

static uint8_t *load(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void wav(const char *p, const int16_t *s, long n, int fs)
{
    FILE *f = fopen(p, "wb");
    uint32_t d = (uint32_t)n * 2, h[11] = { 0x46464952, 36 + d, 0x45564157, 0x20746d66, 16,
        1 | 1 << 16, (uint32_t)fs, (uint32_t)fs * 2, 2 | 16 << 16, 0x61746164, d };
    fwrite(h, 4, 11, f);
    fwrite(s, 2, (size_t)n, f);
    fclose(f);
}

/* The stream as Nuked hears it, tick by tick (same rule as the model). */
typedef struct { const uint8_t *p, *loop; uint32_t wait; } seq;

static void nuked_tick(opl3_chip *ch, seq *q)
{
    if (!q->p) return;
    if (q->wait) { q->wait--; return; }
    for (;;) {
        uint32_t e = (uint32_t)q->p[0] << 8 | q->p[1];
        q->p += 2;
        if (e == 0xFF00) q->p = q->loop;
        else if (e >> 8 == 0) { if (e) { q->wait = e - 1; return; } }
        else OPL3_WriteRegBuffered(ch, (uint16_t)(e >> 8), (uint8_t)e);
    }
}

static double rms_db(const int16_t *s, long n)
{
    double a = 0;
    for (long i = 0; i < n; i++) a += (double)s[i] * s[i];
    a = sqrt(a / (n ? n : 1)) / 32768.0;
    return 20 * log10(a + 1e-9);
}

/* Energy per Hz in [f1, f2) of n samples at fs (Hann, DFT bins at 1/T). */
static double band(const int16_t *x, long n, double fs, double f1, double f2)
{
    double T = n / fs, e = 0;
    int nb = 0;
    for (double f = f1; f < f2; f += 1.0 / T) {
        double w = 2 * M_PI * f / fs, c = cos(w), s1 = sin(w);
        double re = 0, im = 0, cr = 1, ci = 0;
        for (long i = 0; i < n; i++) {
            double h = 0.5 - 0.5 * cos(2 * M_PI * i / n);
            re += h * x[i] * cr;
            im += h * x[i] * ci;
            double t = cr * c - ci * s1;
            ci = cr * s1 + ci * c;
            cr = t;
        }
        e += re * re + im * im;
        nb++;
    }
    return nb ? e / nb / ((double)n * n) : 0;
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: opl2_render muzax.bin SONG SECONDS ours.wav nuked.wav\n"); return 2; }
    long n;
    uint8_t *blob = load(argv[1], &n);
    int song = atoi(argv[2]);
    double secs = atof(argv[3]);
    const uint8_t *tab = blob + 4;
    const uint8_t *init_s = blob + be32(tab), *init_l = blob + be32(tab + 4);
    const uint8_t *song_s = blob + be32(tab + 8 * (1 + song)), *song_l = blob + be32(tab + 8 * (1 + song) + 4);

    /* ours */
    long n1 = (long)(secs * OPL_FS);
    int16_t *a = malloc((size_t)n1 * 2);
    static opl2 c;
    opl2_init(&c);
    opl2_song(&c, init_s, init_l);
    int switched = 0;
    for (long i = 0; i < n1; i++) {
        uint32_t before = c.tick_acc;
        a[i] = (int16_t)opl2_sample(&c);
        if (!switched && c.tick_acc < before) { opl2_song(&c, song_s, song_l); switched = 1; }
    }

    /* Nuked */
    long n2 = (long)(secs * NUKED_FS);
    int16_t *b = malloc((size_t)n2 * 2);
    static opl3_chip ch;
    OPL3_Reset(&ch, NUKED_FS);
    seq q = { init_s, init_l, 0 };
    double tick = NUKED_FS * 6628.0 / 1193182.0, next = tick;
    int first = 1;
    for (long i = 0; i < n2; i++) {
        if (i >= next) {
            nuked_tick(&ch, &q);
            next += tick;
            if (first) { q.p = song_s; q.loop = song_l; q.wait = 0; first = 0; }
        }
        int16_t s2[2];
        OPL3_Generate(&ch, s2);
        int32_t v = s2[0] * 2;
        b[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    wav(argv[4], a, n1, (int)(OPL_FS + 0.5));
    wav(argv[5], b, n2, NUKED_FS);

    /* loudness, 20 ms windows, where Nuked is above -50 dBFS */
    int nw = (int)(secs / 0.02), used = 0;
    double sum = 0, worst = 0;
    for (int w = 0; w < nw; w++) {
        long i1 = (long)(w * 0.02 * OPL_FS), j1 = (long)(w * 0.02 * NUKED_FS);
        double da = rms_db(a + i1, (long)(0.02 * OPL_FS)), db = rms_db(b + j1, (long)(0.02 * NUKED_FS));
        if (db < -50) continue;
        double d = fabs(da - db);
        sum += d; used++;
        if (d > worst) worst = d;
    }
    /* spectra: 0.1 s frames every 2 s, in bands a quarter-octave wide up
     * to 0.45 Fs, energy per bin normalised by the frame length */
    enum { NBAND = 24 };
    double bd[NBAND] = { 0 };
    int bn[NBAND] = { 0 };
    double lsd = 0;
    int frames = 0;
    for (double t = 1.6; t + 0.2 < secs; t += 2.0) {
        long i1 = (long)(t * OPL_FS), j1 = (long)(t * NUKED_FS);
        long na = (long)(0.1 * OPL_FS), nb2 = (long)(0.1 * NUKED_FS);
        double acc = 0; int nb = 0, k = 0;
        for (double f = 100; f * 1.19 < 0.45 * OPL_FS && k < NBAND; f *= 1.19, k++) {
            double ea = band(a + i1, na, OPL_FS, f, f * 1.19), eb = band(b + j1, nb2, NUKED_FS, f, f * 1.19);
            if (eb < 1e-2) continue;
            double d = 10 * log10((ea + 1e-4) / (eb + 1e-4));
            acc += fabs(d); nb++;
            bd[k] += d; bn[k]++;
        }
        if (nb) { lsd += acc / nb; frames++; }
    }
    printf("song %2d: loudness |d| mean %.2f dB, worst %.1f dB (%d windows); spectrum %.2f dB per band (%d frames)\n",
           song, used ? sum / used : 0, worst, used, frames ? lsd / frames : 0, frames);
    printf("   by band (Hz: ours - Nuked dB):");
    double f = 100;
    for (int k = 0; k < NBAND && f * 1.19 < 0.45 * OPL_FS; k++, f *= 1.19)
        if (bn[k]) printf(" %.0f:%+.1f", f, bd[k] / bn[k]);
    putchar('\n');
    return 0;
}
