/* The OPL2 on Jerry, in C: see opl2_model.h.  Every line here has its
 * counterpart in src/dsp_audio.s, in the same order.
 *
 * KEY ON / OFF ON THE EDGE.  The chip notices a key change only when its
 * envelope generator runs (once a sample), so a key-off immediately followed
 * by a key-on would be missed.  MUZAX never does that on the real card: each
 * write costs ~27 us (0x5876: 6 + 35 reads of the port, every write) against
 * a 20 us chip sample, and note_on puts key_off, the A0 write and the B0 write
 * in a row, so the chip sees the key off for a sample or two and retriggers.
 * Here the writes of a tick land between two samples; acting on the edge at
 * the write gives what the card did: off -> release, on -> attack from phase 0.
 */
#include "opl2_model.h"
#include "../../build/opl2_tab.h"

static const int8_t AD_SLOT[32] = {
    0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
    12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};
static const uint8_t CH_MOD[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
static const uint8_t KSLSHIFT[4] = { 8, 1, 2, 0 };

#define ENV_OFF  (0x1F8u << 16)
#define ENV_MAX  (0x1FFu << 16)
#define LIM_NONE 0x7FFFFFFFu

/* The low 32 bits of a (<= 17 bit) x (<= 18 bit) product, as the DSP builds
 * it from 16 x 16 mults. */
static uint32_t mul32(uint32_t a, uint32_t b)
{
    uint32_t al = a & 0xFFFF, ah = a >> 16, bl = b & 0xFFFF, bh = b >> 16;
    return al * bl + ((al * bh + ah * bl) << 16);
}

static void slot_inc(opl2 *c, opl2_slot *s)
{
    uint32_t f = c->fnum[s->ch];
    if (s->vib) {
        int32_t range = (int32_t)((f >> 7) & 7);
        uint32_t p = c->vibpos;
        if (!(p & 3))
            range = 0;
        else if (p & 1)
            range >>= 1;
        range >>= c->vibsh;
        if (p & 4)
            range = -range;
        f = (uint32_t)((int32_t)f + range) & 0xFFFF;
    }
    s->inc = mul32(f << c->block[s->ch], OPL_MULT[s->mult]);
}

/* ATT3 from its inputs.  Recomputed where one of them changes - an
 * envelope step, a refresh, a TL/KSL write, a tremolo step for AM slots - and
 * nowhere else: with a quarter of the envelopes stepping each sample, "every
 * sample" would no longer be what the DSP does. */
static void att3_calc(opl2 *c, opl2_slot *s)
{
    if (s->env == ENV_MAX) {
        s->att3 = OPL_DEAD;
        return;
    }
    uint32_t a = (s->env >> 16) + s->atts + (c->trem & s->amm);
    if (a > 511)
        a = 511;
    s->att3 = a << 3;
}

static void slot_att(opl2 *c, opl2_slot *s)
{
    int32_t ksl = (int32_t)OPL_KSL[c->fnum[s->ch] >> 6] - (int32_t)((8 - c->block[s->ch]) << 5);
    if (ksl < 0)
        ksl = 0;
    s->atts = (s->tl << 2) + ((uint32_t)ksl >> KSLSHIFT[s->kslr]);
    att3_calc(c, s);
}

/* The current state's step and limit, from the slot's rates, and any
 * transition already due applied AT ONCE.  That makes "step 0" mean "nothing
 * will change": the DSP skips such slots without looking further (a decay
 * with DR 0 and SL 0 would otherwise wait in DECAY for ever there, and move
 * to SUSTAIN here).
 *   ATTACK   the step is the attack coefficient (opl2.py AINC)
 *   DECAY    towards SL; at it, SUSTAIN
 *   SUSTAIN  holds if EG-type, else falls at RR
 *   RELEASE  falls at RR
 * Falling below 0x1F8 is the end: the bottom (0x1FF) and no step. */
static void att3_calc(opl2 *c, opl2_slot *s);
static void eg_refresh(opl2 *c, opl2_slot *s)
{
    for (;;) {
        switch (s->egst) {
        case EG_ATTACK:                 /* at 0 the attack is over */
            s->eginc = s->ainc;
            if (s->env) {
                att3_calc(c, s);
                return;
            }
            s->egst = EG_DECAY;
            continue;
        case EG_DECAY:
            s->eginc = s->dincd;
            s->eglim = s->slim;
            break;
        case EG_SUSTAIN:
            s->eginc = s->egt ? 0 : s->dincr;
            s->eglim = ENV_OFF;
            break;
        default:
            s->eginc = s->dincr;
            s->eglim = ENV_OFF;
            break;
        }
        if (s->env < s->eglim) {
            att3_calc(c, s);
            return;
        }
        if (s->egst == EG_DECAY) {
            s->egst = EG_SUSTAIN;
            continue;
        }
        s->env = ENV_MAX;
        s->eginc = 0;
        s->eglim = LIM_NONE;
        att3_calc(c, s);
        return;
    }
}

static void slot_rates(opl2 *c, opl2_slot *s)
{
    uint32_t ch = (uint32_t)s->ch;
    uint32_t ksv = (c->block[ch] << 1) | ((c->fnum[ch] >> (9 - c->nts)) & 1);
    uint32_t ks = ksv >> ((s->ksr ^ 1) << 1);
    uint32_t r;
    r = ks + (s->ar << 2);
    s->ainc = s->ar ? OPL_AINC[r > 63 ? 63 : r] : 0;
    r = ks + (s->dr << 2);
    s->dincd = s->dr ? OPL_DINC[r > 63 ? 63 : r] : 0;
    r = ks + (s->rr << 2);
    s->dincr = s->rr ? OPL_DINC[r > 63 ? 63 : r] : 0;
    r = s->sl << 4;
    s->slim = (r > 0x1F8 ? 0x1F8 : r) << 16;
    eg_refresh(c, s);
}

static void slot_key(opl2 *c, opl2_slot *s, uint32_t key)
{
    uint32_t was = s->key;
    s->key = key;
    if (!was && key) {                  /* on: attack from phase 0 */
        s->ph = 0;
        s->out = 0;                     /* (a dead slot's are left as they were) */
        s->prout = 0;
        s->egst = EG_ATTACK;
        eg_refresh(c, s);
    } else if (was && !key) {           /* off: release */
        s->egst = EG_RELEASE;
        eg_refresh(c, s);
    }
}

/* The waveform as four quadrant entries (idx >> 8 & 3): low byte = the
 * xor that mirrors the quarter-wave index, 0xC00 = this quadrant is silent,
 * bit 31 = negate.  Sine (0) is mirrored in quadrants 1 and 3 and negative in
 * 2 and 3; half-sine (1) silent in 2, 3; abs-sine (2) never negative;
 * quarter-sine (3) unmirrored and silent in 1, 3 (Nuked OPL3's
 * OPL3_EnvelopeCalcSin0..3). */
static void slot_wave(opl2 *c, opl2_slot *s)
{
    static const uint32_t Q[4][4] = {
        { 0, 0xFF, 0x80000000u, 0x800000FFu },
        { 0, 0xFF, 0xC00, 0xCFF },
        { 0, 0xFF, 0, 0xFF },
        { 0, 0xC00, 0, 0xC00 },
    };
    uint32_t wf = c->wse ? s->wf : 0;
    for (int i = 0; i < 4; i++)
        s->q[i] = Q[wf][i];
}

/* How channel ch modulates and mixes (Nuked OPL3's OPL3_ChannelSetupAlg
 * and OPL3_ChannelUpdateRhythm, for an OPL2).  In rhythm mode channels 7
 * and 8 are the rhythm block (opl2_sample) and do not come here. */
static void setup_ch(opl2 *c, uint32_t ch)
{
    opl2_slot *m = &c->s[CH_MOD[ch]];
    uint32_t fb = c->fb[ch], con = c->con[ch];
    m->fbmask = fb ? 0xFFFFFFFFu : 0;
    m->fbsh = fb ? 9 - fb : 0;
    c->modin[ch] = con ? 0 : 0xFFFFFFFFu;
    c->modmix[ch] = con ? 0xFFFFFFFFu : 0;
    c->osh[ch] = 0;
    if (c->rhy & 0x20 && ch == 6) {     /* BD: the carrier only, doubled */
        c->modmix[ch] = 0;
        c->osh[ch] = 1;
    }
}

static void ch_freq(opl2 *c, uint32_t ch)
{
    for (int i = 0; i < 2; i++) {
        opl2_slot *s = &c->s[CH_MOD[ch] + 3 * i];
        slot_inc(c, s);
        slot_att(c, s);
        slot_rates(c, s);
    }
}

void opl2_write(opl2 *c, uint32_t reg, uint32_t v)
{
    reg &= 0xFF;
    v &= 0xFF;
    uint32_t grp = reg & 0xE0;
    if ((grp >= 0x20 && grp <= 0x80) || grp == 0xE0) {
        int n = AD_SLOT[reg & 0x1F];
        if (n < 0)
            return;
        opl2_slot *s = &c->s[n];
        switch (grp) {
        case 0x20:
            s->am = v >> 7;
            s->amm = s->am ? 0xFFFFFFFFu : 0;
            s->vib = (v >> 6) & 1;
            s->egt = (v >> 5) & 1;
            s->ksr = (v >> 4) & 1;
            s->mult = v & 15;
            slot_inc(c, s);
            slot_rates(c, s);
            break;
        case 0x40:
            s->kslr = v >> 6;
            s->tl = v & 63;
            slot_att(c, s);
            break;
        case 0x60:
            s->ar = v >> 4;
            s->dr = v & 15;
            slot_rates(c, s);
            break;
        case 0x80:
            s->sl = v >> 4;
            if (s->sl == 15)
                s->sl = 31;
            s->rr = v & 15;
            slot_rates(c, s);
            break;
        default:
            s->wf = v & 3;
            slot_wave(c, s);
            break;
        }
        return;
    }
    if (reg == 0x01) {
        c->wse = v & 0x20;
        return;
    }
    if (reg == 0x08) {
        c->nts = (v >> 6) & 1;
        return;
    }
    if (reg == 0xBD) {
        c->tremsh = (((v >> 7) ^ 1) << 1) + 2;
        c->vibsh = ((v >> 6) & 1) ^ 1;
        c->rhy = v & 0x3F;
        setup_ch(c, 6);
        setup_ch(c, 7);
        setup_ch(c, 8);
        uint32_t on = c->rhy & 0x20 ? c->rhy : 0;
        slot_key(c, &c->s[13], (c->s[13].key & 1) | (on & 0x01 ? 2 : 0));   /* HH */
        slot_key(c, &c->s[17], (c->s[17].key & 1) | (on & 0x02 ? 2 : 0));   /* TC */
        slot_key(c, &c->s[14], (c->s[14].key & 1) | (on & 0x04 ? 2 : 0));   /* TOM */
        slot_key(c, &c->s[16], (c->s[16].key & 1) | (on & 0x08 ? 2 : 0));   /* SD */
        slot_key(c, &c->s[12], (c->s[12].key & 1) | (on & 0x10 ? 2 : 0));   /* BD */
        slot_key(c, &c->s[15], (c->s[15].key & 1) | (on & 0x10 ? 2 : 0));
        return;
    }
    uint32_t ch = reg & 0x0F;
    if (ch > 8)
        return;
    switch (reg & 0xF0) {
    case 0xA0:
        c->fnum[ch] = (c->fnum[ch] & 0x300) | v;
        ch_freq(c, ch);
        break;
    case 0xB0:
        c->fnum[ch] = (c->fnum[ch] & 0xFF) | ((v & 3) << 8);
        c->block[ch] = (v >> 2) & 7;
        ch_freq(c, ch);
        for (int i = 0; i < 2; i++) {
            opl2_slot *s = &c->s[CH_MOD[ch] + 3 * i];
            slot_key(c, s, (s->key & 2) | (v & 0x20 ? 1 : 0));
        }
        break;
    case 0xC0:
        c->fb[ch] = (v >> 1) & 7;
        c->con[ch] = v & 1;
        setup_ch(c, ch);
        break;
    }
}

void opl2_init(opl2 *c)
{
    for (unsigned i = 0; i < sizeof *c; i++)
        ((uint8_t *)c)[i] = 0;
    for (int ch = 0; ch < 9; ch++) {
        c->s[CH_MOD[ch]].ch = ch;
        c->s[CH_MOD[ch] + 3].ch = ch;
    }
    c->tremsh = 4;
    c->vibsh = 1;
    c->noise = 1;
    for (int i = 0; i < 18; i++) {
        opl2_slot *s = &c->s[i];
        s->env = ENV_MAX;
        s->att3 = OPL_DEAD;
        s->egst = EG_RELEASE;
        slot_wave(c, s);
        slot_rates(c, s);
        slot_inc(c, s);
        slot_att(c, s);
    }
    for (uint32_t ch = 0; ch < 9; ch++)
        setup_ch(c, ch);
}

void opl2_song(opl2 *c, const uint8_t *start, const uint8_t *loop)
{
    c->req_ptr = start;
    c->req_loop = loop;
    c->req_mus++;
}

void opl2_sfx(opl2 *c, const int8_t *pcm, uint32_t n)
{
    c->req_sfx = pcm;
    c->req_len = n;
    c->req_snd++;
}

/* One MUZAX tick: the 68000's posts, then the stream's writes until a wait
 * (tools/muzax.py). */
static void seq_tick(opl2 *c)
{
    if (c->req_mus != c->mus_seen) {
        c->mus_seen = c->req_mus;
        c->seq_ptr = c->req_ptr;
        c->seq_loop = c->req_loop;
        c->seq_wait = 0;
    }
    if (c->req_snd != c->snd_seen) {
        c->snd_seen = c->req_snd;
        c->sfx_ptr = c->req_sfx;
        c->sfx_rem = c->req_len;
    }
    if (c->seq_wait) {
        c->seq_wait--;
        return;
    }
    if (!c->seq_ptr)
        return;
    for (;;) {
        uint32_t e = (uint32_t)c->seq_ptr[0] << 8 | c->seq_ptr[1];
        c->seq_ptr += 2;
        if (e == 0xFF00) {
            c->seq_ptr = c->seq_loop;
        } else if (e >> 8 == 0) {
            if (e) {
                c->seq_wait = e - 1;
                return;
            }
        } else {
            opl2_write(c, e >> 8, e & 0xFF);
        }
    }
}

static void lfo_step(opl2 *c)
{
    c->tpos++;
    if (c->tpos == 210)
        c->tpos = 0;
    c->trem = (c->tpos < 105 ? c->tpos : 210 - c->tpos) >> c->tremsh;
    c->vcnt++;
    if (c->vcnt == 16) {
        c->vcnt = 0;
        c->vibpos = (c->vibpos + 1) & 7;
        for (int i = 0; i < 18; i++)
            if (c->s[i].vib)
                slot_inc(c, &c->s[i]);
    }
    for (int i = 0; i < 18; i++)        /* the tremolo moved: AM slots' ATT3 */
        if (c->s[i].amm)
            att3_calc(c, &c->s[i]);
}

static void eg_step(opl2 *c, opl2_slot *s)
{
    uint32_t env = s->env;
    if (s->egst == EG_ATTACK) {
        uint32_t x = env + 0x10000;
        uint32_t dec = (((x >> 10) & 0xFFFF) * (s->eginc & 0xFFFF)) >> 6;
        env -= dec;
        s->env = env;
        if ((int32_t)env <= 0) {
            s->env = 0;
            s->egst = EG_DECAY;
            eg_refresh(c, s);
        }
    } else {
        env += s->eginc;
        s->env = env;
        if (env >= s->eglim) {
            if (s->egst == EG_DECAY)
                s->egst = EG_SUSTAIN;
            eg_refresh(c, s);
        }
    }
    att3_calc(c, s);
}

static int32_t sat16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* One operator's output at wave index idx (any int: masked here). */
static int32_t op_wave(const opl2_slot *s, uint32_t idx)
{
    uint32_t e = s->q[(idx >> 8) & 3];
    uint32_t level = OPL_LOGSIN[(idx ^ e) & 0xFF] + s->att3 + (e & 0xC00);
    /* level <= 0x859 + 0xBFF + 0xC00 = 0x2058: a shift of up to 32, which
     * the DSP's sh does (0 at 32, JaguarDocs hw/06) and C does not. */
    uint32_t sh = level >> 8;
    int32_t o = sh > 31 ? 0 : (int32_t)(OPL_EXP[level & 0xFF] >> sh);
    return o ^ ((int32_t)e >> 31);
}

/* The silent skip: nothing sounds, but the phase runs on.  Stopping it too
 * looked free and was not: a modulator that starts below the threshold (a
 * slow attack) would come in out of phase with its carrier, and in FM the
 * phase between the two IS the timbre (+0.5 dB of loudness error, +2 dB of
 * spectrum on song 0 against Nuked). */
static int silent(opl2_slot *s)
{
    if (s->att3 < OPL_SILENT)
        return 0;
    if (s->att3 == OPL_DEAD)            /* untouched: the key-on resets it */
        return 1;
    s->ph += s->inc;
    s->out = 0;
    s->prout = 0;
    return 1;
}

static int32_t op_mod(opl2_slot *s)
{
    if (silent(s))
        return 0;
    int32_t o = s->out;
    int32_t fb = ((o + s->prout) >> s->fbsh) & (int32_t)s->fbmask;
    s->prout = o;
    uint32_t ph = s->ph;
    s->ph = ph + s->inc;
    o = op_wave(s, (ph >> 22) + (uint32_t)fb);
    s->out = o;
    return o;
}

static int32_t op_car(opl2_slot *s, int32_t modin)
{
    if (silent(s))
        return 0;
    uint32_t ph = s->ph;
    s->ph = ph + s->inc;
    int32_t o = op_wave(s, (ph >> 22) + (uint32_t)modin);
    s->out = o;
    return o;
}

/* A rhythm operator: its wave index comes from the generator (TOM: from its
 * own phase, read before this sample's step), and its phase always runs -
 * HH's and TC's feed the generator; for the other two it is simply cheaper
 * for the DSP to treat the four alike. */
static int32_t op_rhy(opl2_slot *s, uint32_t idx)
{
    s->ph += s->inc;
    if (s->att3 >= OPL_SILENT) {
        s->out = 0;
        s->prout = 0;
        return 0;
    }
    int32_t o = op_wave(s, idx);
    s->out = o;
    return o;
}

int32_t opl2_sample(opl2 *c)
{
    uint32_t t = c->tick_acc + OPL_TICK_INC;
    if (t < c->tick_acc)
        seq_tick(c);
    c->tick_acc = t;
    t = c->lfo_acc + OPL_LFO_INC;
    if (t < c->lfo_acc)
        lfo_step(c);
    c->lfo_acc = t;

    /* A quarter of the envelopes each sample: slot k on samples k mod 4
     * (tools/opl2.py EG_EVERY). */
    for (int i = c->count & 3; i < 18; i += 4)
        eg_step(c, &c->s[i]);

    /* The rhythm generator (Nuked OPL3's OPL3_PhaseGenerate), from the
     * phases of HH and TC at the start of the sample. */
    uint32_t hh = c->s[13].ph >> 22, tc = c->s[17].ph >> 22, nz = c->noise & 1;
    uint32_t x = (((hh >> 2) ^ (hh >> 7)) | ((hh >> 3) ^ (tc >> 5)) | ((tc >> 3) ^ (tc >> 5))) & 1;
    c->ridx[1] = (x << 9) | ((x ^ nz) ? 0xD0 : 0x34);
    c->ridx[2] = (((hh >> 8) & 1) << 9) | ((((hh >> 8) ^ nz) & 1) << 8);
    c->ridx[3] = (x << 9) | 0x80;
    c->ridx[0] = c->s[14].ph >> 22;     /* TOM: its own phase */

    int32_t mix = 0;
    int nch = c->rhy & 0x20 ? 7 : 9;
    for (int ch = 0; ch < nch; ch++) {
        opl2_slot *m = &c->s[CH_MOD[ch]], *k = m + 3;
        int32_t mo = op_mod(m);
        int32_t ko = op_car(k, mo & (int32_t)c->modin[ch]);
        mix += ((mo & (int32_t)c->modmix[ch]) + ko) << c->osh[ch];
    }
    if (nch == 7) {                     /* HH, TOM, SD, TC: each doubled */
        int32_t r = op_rhy(&c->s[13], c->ridx[1]);
        r += op_rhy(&c->s[14], c->ridx[0]);
        r += op_rhy(&c->s[16], c->ridx[2]);
        r += op_rhy(&c->s[17], c->ridx[3]);
        mix += r << 1;
    }
    uint32_t n = c->noise;
    c->noise = (n >> 1) | ((((n >> 14) ^ n) & 1) << 22);

    c->count++;
    int32_t smp = sat16(mix) << 1;
    if (c->sfx_rem) {
        smp += (int32_t)*c->sfx_ptr++ << 8;     /* sr2jag.py SFX_GAIN */
        c->sfx_rem--;
    }
    return sat16(smp);
}
