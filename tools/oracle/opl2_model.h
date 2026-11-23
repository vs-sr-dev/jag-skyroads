/* The OPL2 on Jerry, in C: the SPECIFICATION of src/dsp_audio.s.
 *
 * Integer only, 32-bit, the same tables (build/opl2_tab.h, from
 * tools/opl2.py) and the same order of operations as the DSP code, so that
 * the DSP binary run in tools/oracle/jrisc.py must give the same samples bit
 * for bit (tools/oracle/dsp_check.py).  How close it comes to a real YM3812
 * is a separate question, answered against Nuked OPL3 by
 * tools/oracle/opl2_render.c (envelopes and pitch, not samples: the rates
 * differ).
 *
 * What it is: a YM3812 register file rescaled to Jerry's I2S rate Fs.  Phase
 * in 32 bits (the wave index in the top 10), envelopes in 16.16 per Jaguar
 * sample with the chip's average rates, attack as an exponential calibrated
 * on the chip's attack time, LFOs stepped by a 32-bit accumulator of chip
 * time.  Key on/off act on the EDGE, at the write (see opl2_model.c).
 */
#ifndef OPL2_MODEL_H
#define OPL2_MODEL_H

#include <stdint.h>

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

/* An operator whose level is this far down is exactly silent: level >=
 * 0xC00 shifts the doubled exp (<= 0xFF4) right by 12 or more.  Its wave is
 * not computed: out = prout = 0, and only its phase advances.  (The chip
 * gives -1, not 0, in a silent sine's negative half: ten silent operators
 * are square waves of +-20 LSB, -64 dBFS, which shows only in a near-empty
 * band of the spectrum comparison - song 0, 169-239 Hz - and not to the
 * ear.) */
#define OPL_SILENT 0xC00u

/* An operator at the bottom of its envelope (0x1FF) is DEAD: only a key-on
 * can bring it back, and a key-on zeroes its phase, so not even the phase
 * needs to run.  Its ATT3 says so (0x1000, above anything a live operator
 * reaches).  HH's and TC's phases run regardless: the rhythm generator reads
 * them. */
#define OPL_DEAD 0x1000u

typedef struct {
    /* the synthesis, every sample */
    uint32_t att3;              /* envelope + level, << 3: added to the log-sine */
    uint32_t ph, inc;
    int32_t out, prout;
    uint32_t fbsh, fbmask;      /* feedback: (out + prout) >> fbsh, masked */
    uint32_t q[4];              /* the waveform by quadrant: xor | silent 0xC00 | negate bit 31 */
    /* the envelope, every sample */
    uint32_t env, egst, eginc, eglim, ainc, dincd, dincr, slim, atts, amm;
    /* registers */
    uint32_t key, am, vib, egt, ksr, mult, ar, dr, sl, rr, kslr, tl, wf;
    int ch;
} opl2_slot;

typedef struct {
    opl2_slot s[18];
    uint32_t fnum[9], block[9], fb[9], con[9];
    /* how a channel mixes: the carrier's modulation is the modulator's out
     * & modin; the mix gets (mod & modmix) + car, << osh */
    uint32_t modin[9], modmix[9], osh[9];
    uint32_t wse, nts, rhy, tremsh, vibsh;
    uint32_t lfo_acc, tpos, trem, vcnt, vibpos;
    uint32_t noise;
    uint32_t ridx[4];
    uint32_t count;                     /* samples produced */
    /* the sequencer: a 16-bit entry stream, see tools/muzax.py */
    uint32_t tick_acc, seq_wait;
    const uint8_t *seq_ptr, *seq_loop;
    /* the effect: signed 8-bit samples at Fs */
    const int8_t *sfx_ptr;
    uint32_t sfx_rem;
    /* what the 68000 posted (MUS_* / SND_*), taken at the next tick */
    const uint8_t *req_ptr, *req_loop;
    const int8_t *req_sfx;
    uint32_t req_len, req_mus, req_snd, mus_seen, snd_seen;
} opl2;

void opl2_init(opl2 *c);
void opl2_write(opl2 *c, uint32_t reg, uint32_t val);
/* The 68000's posts: taken at the next MUZAX tick, as the DSP does. */
void opl2_song(opl2 *c, const uint8_t *start, const uint8_t *loop);
void opl2_sfx(opl2 *c, const int8_t *pcm, uint32_t n);
int32_t opl2_sample(opl2 *c);   /* what goes into the ring: a 16-bit signed sample */

#endif
