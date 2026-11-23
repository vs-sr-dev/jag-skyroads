;===========================================================================
; src/dsp_audio.s - Jerry: an OPL2 synthesised on the DSP, MUZAX's register
; streams, and the effects.
;
; WHAT RUNS HERE
; --------------
; SkyRoads' music is AdLib: MUZAX writing OPL2 registers at 180 Hz.  Its
; driver ran at BUILD time (tools/muzax.py): the cartridge carries the
; register writes, tick by tick, and this code is the chip.  It reads a
; stream (MUS_ADR/MUS_LOOP, switched by MUS_SEQ), applies each tick's writes
; at 180.02 Hz counted in samples, and synthesises the YM3812's 18
; operators - six melodic channels and the rhythm section (bass drum, snare,
; tom, cymbal, hi-hat) - at the I2S rate.  The one effect the original's
; Sound Blaster could play at a time is added on top (SND_ADR/SND_LEN,
; switched by SND_SEQ), as skyroads-port mixes it.
;
; THE SPECIFICATION IS tools/oracle/opl2_model.c.  Every routine here has
; its counterpart there, in the same order and with the same integer
; arithmetic; tools/oracle/dsp_check.py runs THIS binary in a JRISC
; interpreter and wants the same samples, bit for bit.  How close the model
; is to a real YM3812 is measured separately against Nuked OPL3
; (tools/oracle/opl2_render.c).
;
; THE PLUMBING IS KEEN DREAMS' (src/dsp_audio.s there, verified on hardware):
; the power-up order on the 68000 side, the two banks (bank 0: init + I2S
; interrupt; bank 1: the idle loop that produces), the I2S switched on from
; the idle loop when the ring is first full, the ISR's return sequence,
; silence produced as zeros.  JaguarDocs hw/07 is the rule book.
;
; WHERE THE TIME GOES (Fs = 15,980 Hz at SCLK 25: ~1,580 clk a sample after
; the ISR).  An operator costs ~50 clk; 18 of them do not fit with their
; envelopes, so:
;   - an operator whose level is down past 0xC00 is exactly silent and its
;     wave is not computed (its phase still runs: in FM the phase between
;     modulator and carrier is the timbre);
;   - an envelope with nothing to do (step 0: holding, or at the bottom) is
;     skipped with one test - opl2_model.c's eg_refresh applies every due
;     transition at once so that "step 0" really means "nothing changes";
;   - the level an operator adds to its log-sine (ATT3) is recomputed only
;     when one of its inputs changes;
;   - modulator and carrier are computed per CHANNEL, the modulator's output
;     going to the carrier in a register.
; The retail songs keep at most 16 operators audible (avg ~11).
;
; RULES THIS CODE FOLLOWS (JaguarDocs):
;   - code and tables in local RAM; tables are longs (hw/06 rule 4);
;   - every external load is used (or rX,rX) before anything else touches
;     the register (hw/06 rule 2), and none follows another unused (hw/07
;     rule 12);
;   - a jump target is loaded BEFORE the compare (tools/oracle/jrisc.py
;     breaks the flags on movei, as a first TOM was seen to);
;   - every jump has its delay-slot nop;
;   - bank 1 never touches r30/r31 (the interrupt's).
;===========================================================================

DSP_BUF_LEN     EQU     64
DSP_BUF_MASK    EQU     DSP_BUF_LEN-1
DSP_SMODE       EQU     $15                     ; mode16, falling edge

; --- a slot (operator): 28 longs.  The four quadrant entries come first:
;     the wave code reads them with load (r14+Rn), Rn = quadrant * 4.
S_Q0            EQU     0
S_ATT3          EQU     4       ; level added to the log-sine (<< 3)
S_PH            EQU     5
S_INC           EQU     6
S_OUT           EQU     7
S_PROUT         EQU     8
S_FBSH          EQU     9
S_FBMASK        EQU     10
S_ENV           EQU     11      ; 16.16, 0 = loudest, $1FF<<16 = the bottom
S_EGINC         EQU     12      ; step per sample (the attack: its coefficient)
S_EGLIM         EQU     13      ; at or past it, the next state
S_EGST          EQU     14      ; 0 attack, 1 decay, 2 sustain, 3 release
S_ATTS          EQU     15      ; TL*4 + KSL
S_AMM           EQU     16      ; tremolo: -1 or 0
S_AINC          EQU     17
S_DINCD         EQU     18
S_DINCR         EQU     19
S_SLIM          EQU     20
S_KEY           EQU     21      ; bit 0 the channel's key, bit 1 the drum's
S_R20           EQU     22      ; the registers as written
S_R40           EQU     23
S_R60           EQU     24
S_R80           EQU     25
S_RE0           EQU     26
S_CH            EQU     27      ; its channel's record
SLOT_SIZE       EQU     112

; --- a channel: 8 longs
C_SLOT          EQU     0       ; its modulator
C_CAR           EQU     1       ; its carrier
C_FNUM          EQU     2
C_BLOCK         EQU     3
C_FB            EQU     4
C_CON           EQU     5
C_MODIN         EQU     6       ; carrier's modulation = modulator & MODIN
C_MODMIX        EQU     7       ; ... and the mix gets (mod & MODMIX) + car,
C_OSH           EQU     8       ; << -OSH (0, or -1 for the doubled BD)
CH_SIZE         EQU     36

EG_ATTACK       EQU     0
EG_DECAY        EQU     1
EG_SUSTAIN      EQU     2
EG_RELEASE      EQU     3
ENV_OFF         EQU     ($1F8<<16)
ENV_MAX         EQU     ($1FF<<16)
LIM_NONE        EQU     $7FFFFFFF
OPL_DEAD        EQU     $1000   ; ATT3 of an operator at the bottom: skip it all

; The slow tables in DRAM (InitDspAudio copies opl2_rom.inc there).
T_DINC          EQU     OPL_TABS
T_AINC          EQU     OPL_TABS+256
T_MULT          EQU     OPL_TABS+512
T_KSL           EQU     OPL_TABS+576

;---------------------------------------------------------------------------
; The 68000 side.
;---------------------------------------------------------------------------
    .68000

; InitDspAudio - Keen Dreams' order, not to be changed: DSP stopped, byte
; order, rate, mode, latches clean, cells, copy, start.  The whole 8 KB is
; written (the blob, then zeros): on hardware it holds what the previous ROM
; left (JaguarDocs hw/07 rule 4).
InitDspAudio:
    movem.l d0/a0/a1, -(sp)
    move.l  #0, D_CTRL
    move.l  #$00050005, D_END
    move.l  #OPL_SCLK, SCLK
    move.l  #DSP_SMODE, SMODE
    move.l  #$23E00, D_FLAGS

    ; Every cell the DSP reads, before it starts.  INIT is the first stream
    ; (the driver's boot init); the 68000 posts songs from the first frame.
    moveq   #0, d0
    move.l  d0, MUS_SEQ
    move.l  d0, SND_SEQ
    move.l  d0, SND_ADR
    move.l  d0, SND_LEN
    move.l  d0, SND_STARVE
    move.l  d0, SND_IRQ
    move.l  d0, SND_DONE
    move.l  d0, SND_SPIN
    move.l  ROM_MUZAX+4, d0                 ; stream 0 = INIT
    add.l   #ROM_MUZAX, d0
    move.l  d0, MUS_ADR
    move.l  ROM_MUZAX+8, d0
    add.l   #ROM_MUZAX, d0
    move.l  d0, MUS_LOOP
    move.l  #1, MUS_SEQ                     ; the DSP starts having seen 0

    move.l  #DspAudioRom, a0
    move.l  #D_RAM, a1
    move.w  #((DspAudioRomEnd-DspAudioRom)+3)/4-1, d0
.ida_copy:
    move.l  (a0)+, (a1)+
    dbra    d0, .ida_copy
    moveq   #0, d0
.ida_zero:
    cmpa.l  #D_ENDRAM, a1
    bhs.s   .ida_go
    move.l  d0, (a1)+
    bra.s   .ida_zero
.ida_go:
    ; the slow tables, where the DSP finds them (OPL_TABS: see map)
    move.l  #opl_dinc, a0
    move.l  #OPL_TABS, a1
    move.w  #(640/4)-1, d0
.ida_tabs:
    move.l  (a0)+, (a1)+
    dbra    d0, .ida_tabs

    move.l  #D_RAM, D_PC
    move.l  #DSPGO, D_CTRL
    movem.l (sp)+, d0/a0/a1
    rts

; The tables only the DSP's slow paths read (rates, multiplier, KSL): in
; DRAM with the 68000's code, not in Jerry's 8 KB (tools/opl2.py).
    .include "opl2_rom.inc"

;---------------------------------------------------------------------------
; The DSP code.  Emitted into the cartridge at DspAudioRom, runs at D_RAM.
;---------------------------------------------------------------------------
    .phrase
DspAudioRom:
    .dsp
    .org    D_RAM

; --- Bank 0: the interrupt (Keen Dreams) ---
ALT_RISC_FLAGS  .equr   r13
ALT_FLAGSTEMP   .equr   r14
ALT_PCTEMP      .equr   r15
ALT_RD          .equr   r16
ALT_WR          .equr   r17
ALT_LDAC        .equr   r18
ALT_RDAC        .equr   r19
ALT_STACKRESET  .equr   r20
ALT_SAMPLE      .equr   r21
ALT_RDP         .equr   r22
ALT_WRP         .equr   r23
ALT_BUF         .equr   r24
ALT_TMP         .equr   r25
ALT_MASK        .equr   r26
ALT_STARVE      .equr   r27
ALT_STARVEP     .equr   r28

; --- $F1B000: INT0 / start ---
    movei   #dsp_audio_init, r0
    jump    T, (r0)
    nop
    nop
    nop
    nop

; --- $F1B010: INT1 = I2S.  One stereo pair per interrupt. ---
int_1:
    load    (ALT_RISC_FLAGS), ALT_FLAGSTEMP
    load    (ALT_WRP), ALT_WR
    move    ALT_RD, ALT_TMP
    and     ALT_MASK, ALT_TMP
    shlq    #2, ALT_TMP
    add     ALT_BUF, ALT_TMP
    cmp     ALT_WR, ALT_RD                  ; rd == wr -> buffer empty
    jr      EQ, .starved
    nop
    load    (ALT_TMP), ALT_SAMPLE
    addq    #1, ALT_RD
    jr      T, .emit
    store   ALT_RD, (ALT_RDP)               ; delay slot: it does execute
.starved:
    addq    #1, ALT_STARVE
    store   ALT_STARVE, (ALT_STARVEP)
.emit:
    ; --- Return.  The original's sequence, not to be touched. ---
    bclr    #3, ALT_FLAGSTEMP
    bset    #10, ALT_FLAGSTEMP
    load    (r31), ALT_PCTEMP
    move    ALT_STACKRESET, r31
    addq    #2, ALT_PCTEMP
    store   ALT_SAMPLE, (ALT_LDAC)
    store   ALT_SAMPLE, (ALT_RDAC)
    jump    T, (ALT_PCTEMP)
    store   ALT_FLAGSTEMP, (ALT_RISC_FLAGS)

    .long
int_stack:
    dc.l    0, 0, 0, 0
snd_rd:
    dc.l    0                               ; ISR writes, loop reads
snd_wr:
    dc.l    0                               ; loop writes, ISR reads
snd_starve:
    dc.l    0                               ; ISR writes, loop reads
i2s_armed:
    dc.l    0

; --- The power-up, once, from INT0 (bank 0) ---
dsp_audio_init:
    movei   #D_FLAGS, ALT_RISC_FLAGS
    movei   #L_DAC, ALT_LDAC
    movei   #R_DAC, ALT_RDAC
    movei   #int_stack+8, ALT_STACKRESET
    movei   #snd_rd, ALT_RDP
    movei   #snd_wr, ALT_WRP
    movei   #snd_starve, ALT_STARVEP
    movei   #snd_buf, ALT_BUF
    movei   #DSP_BUF_MASK, ALT_MASK
    moveq   #0, ALT_RD
    moveq   #0, ALT_WR
    moveq   #0, ALT_SAMPLE
    moveq   #0, ALT_STARVE
    move    ALT_STACKRESET, r31
    movei   #$00004000, r0                  ; REGPAGE, I2S not yet
    store   r0, (ALT_RISC_FLAGS)
    nop
    nop
    nop
    nop

;---------------------------------------------------------------------------
; Bank 1.  Registers kept across the whole loop:
;   r29 LOGEXP   r28 $FFFF    r27 $FF      r26 $C00 (silent)  r25 tremolo
;   r24 mix      r23 wr       r22 &snd_wr  r21 &snd_rd  r20 ring  r19 63
;   r18 tick accumulator      r17 LFO accumulator       r16 noise
; r14/r15 slot/channel pointers, r0-r13 scratch.  The routines called on
; the slow paths link through fixed registers by depth (r12 from the loop,
; r11 opl_write, r10 ch_freq, r9 the slot helpers, r8 eg_refresh, r7
; att3_calc) and may clobber r0-r6, r13-r15.
;---------------------------------------------------------------------------
dsp_idle_setup:
    movei   #opl_logexp, r29
    movei   #$FFFF, r28
    movei   #$FF, r27
    movei   #$C00, r26
    moveq   #0, r25
    moveq   #0, r23
    movei   #snd_wr, r22
    movei   #snd_rd, r21
    movei   #snd_buf, r20
    movei   #DSP_BUF_MASK, r19
    moveq   #0, r18
    moveq   #0, r17
    moveq   #1, r16

    ; The chip at power-up (opl2_init): every slot at the bottom, released,
    ; silent, sine; every channel modulating its carrier, no feedback.
    movei   #slots, r14
    moveq   #0, r13                         ; slot number
    moveq   #18, r12                        ; slots left
.is_slot:
    movei   #ENV_MAX, r1
    store   r1, (r14+S_ENV)
    moveq   #EG_RELEASE, r1
    store   r1, (r14+S_EGST)
    movei   #LIM_NONE, r1
    store   r1, (r14+S_EGLIM)
    movei   #OPL_DEAD, r1
    store   r1, (r14+S_ATT3)
    moveq   #0, r1
    store   r1, (r14)                       ; the sine's quadrants
    movei   #$FF, r1
    store   r1, (r14+1)
    movei   #$80000000, r1
    store   r1, (r14+2)
    movei   #$800000FF, r1
    store   r1, (r14+3)
    ; its channel: slot n of 0..17 -> (n / 6) * 3 + n % 3
    movei   #slot_ch, r1
    move    r13, r2
    shlq    #2, r2
    add     r2, r1
    load    (r1), r1
    store   r1, (r14+S_CH)
    movei   #SLOT_SIZE, r1
    add     r1, r14
    addq    #1, r13
    movei   #.is_slot, r0
    subq    #1, r12
    jump    NE, (r0)
    nop

    ; Channels: their slots; MODIN all ones.
    movei   #chans, r15
    movei   #ch_mod, r2                     ; table: the 9 modulators
    moveq   #9, r13
.ic_loop:
    load    (r2), r1
    store   r1, (r15)
    movei   #(3*SLOT_SIZE), r3
    add     r3, r1
    store   r1, (r15+C_CAR)
    moveq   #0, r1
    subq    #1, r1
    store   r1, (r15+C_MODIN)
    addq    #4, r2
    movei   #CH_SIZE, r1
    add     r1, r15
    movei   #.ic_loop, r0
    subq    #1, r13
    jump    NE, (r0)
    nop

dsp_idle:
    ; --- Is there room?  full = wr - rd = 64: bit 6.
    load    (r21), r1
    movei   #dsp_spin, r0
    move    r23, r2
    sub     r1, r2
    btst    #6, r2
    jump    NE, (r0)
    nop

    ; --- The MUZAX tick and the LFO: 32-bit accumulators, the carry is the
    ;     event (tools/opl2.py OPL_TICK_INC / OPL_LFO_INC).
    movei   #OPL_TICK_INC, r1
    movei   #.no_tick, r0
    add     r1, r18
    jump    CC, (r0)
    nop
    movei   #.no_tick, r12
    movei   #seq_tick, r0
    jump    T, (r0)
    nop
.no_tick:
    movei   #OPL_LFO_INC, r1
    movei   #.no_lfo, r0
    add     r1, r17
    jump    CC, (r0)
    nop
    movei   #.no_lfo, r12
    movei   #lfo_step, r0
    jump    T, (r0)
    nop
.no_lfo:

prof_eg:                                    ; (labels for dsp_check's profile)
    ; --- The envelopes (opl2_model.c eg_step) ---
    ; A quarter of them each sample: slot k on the samples k mod 4 (the
    ; tables are per 4-sample step, tools/opl2.py EG_EVERY).  A slot whose
    ; step is 0 has nothing to do (eg_refresh applies every due transition
    ; at once): one load, one untaken jr.
    moveq   #3, r1
    and     r23, r1                         ; this sample's quarter
    moveq   #5, r13                         ; quarters 0, 1: five slots
    btst    #1, r1
    jr      EQ, .eg_n5
    nop
    moveq   #4, r13                         ; quarters 2, 3: four
.eg_n5:
    movei   #SLOT_SIZE, r11
    mult    r1, r11
    movei   #slots, r14
    add     r11, r14
    movei   #(4*SLOT_SIZE), r11
.eg_loop:
    load    (r14+S_EGINC), r1
    cmpq    #0, r1
    jr      NE, .eg_act
    nop
.eg_next:
    add     r11, r14
    subq    #1, r13
    jr      NE, .eg_loop
    nop
    movei   #.eg_done, r0
    jump    T, (r0)
    nop
.eg_act:
    load    (r14+S_EGST), r2
    load    (r14+S_ENV), r3
    movei   #.eg_atk, r0
    cmpq    #EG_ATTACK, r2
    jump    EQ, (r0)
    nop
    add     r1, r3
    load    (r14+S_EGLIM), r4
    store   r3, (r14+S_ENV)
    movei   #.eg_att3, r0
    cmp     r4, r3                          ; env - lim: CS while below
    jump    CS, (r0)
    nop
    cmpq    #EG_DECAY, r2                   ; due: DECAY -> SUSTAIN ...
    jr      NE, .eg_ref
    nop
    moveq   #EG_SUSTAIN, r2
    store   r2, (r14+S_EGST)
.eg_ref:
    movei   #.eg_next, r8                   ; ... and the refresh (which
    movei   #eg_refresh, r0                 ; recomputes ATT3 too)
    jump    T, (r0)
    nop
.eg_atk:
    ; x = env + 1.0; env -= ((x >> 10) * coefficient) >> 6
    movei   #$10000, r4
    add     r3, r4
    shrq    #10, r4
    mult    r1, r4
    shrq    #6, r4
    sub     r4, r3
    store   r3, (r14+S_ENV)
    move    r3, r4
    subq    #1, r4
    jr      PL, .eg_att3                    ; still above 0
    nop
    moveq   #0, r3
    store   r3, (r14+S_ENV)
    moveq   #EG_DECAY, r2
    store   r2, (r14+S_EGST)
    movei   #.eg_next, r8
    movei   #eg_refresh, r0
    jump    T, (r0)
    nop
.eg_att3:
    ; ATT3 = min((env >> 16) + ATTS + (tremolo & AMM), 511) << 3 (env is
    ; never at the bottom here: reaching it goes through eg_refresh)
    load    (r14+S_ATTS), r5
    load    (r14+S_AMM), r6
    shrq    #16, r3
    and     r25, r6
    add     r5, r3
    movei   #511, r5
    add     r6, r3
    cmp     r5, r3                          ; a - 511: CS while below
    jr      CS, .eg_a_ok
    nop
    move    r5, r3
.eg_a_ok:
    shlq    #3, r3
    store   r3, (r14+S_ATT3)
    movei   #.eg_next, r0
    jump    T, (r0)
    nop
.eg_done:
prof_ch:

    ; --- The channels (opl2_model.c opl2_sample) ---
    movei   #opl_rhy, r1
    load    (r1), r1
    moveq   #0, r24                         ; mix
    movei   #chans, r15
    moveq   #9, r13
    movei   #.ch_loop, r0
    btst    #5, r1
    jump    EQ, (r0)
    nop
    moveq   #7, r13

    ; The rhythm generator, from HH's and TC's phases (Nuked OPL3).
    movei   #slots+(13*SLOT_SIZE)+(S_PH*4), r2
    load    (r2), r2
    movei   #slots+(17*SLOT_SIZE)+(S_PH*4), r3
    load    (r3), r3
    shrq    #22, r2                         ; hh
    shrq    #22, r3                         ; tc
    move    r2, r4
    shrq    #2, r4
    move    r2, r5
    shrq    #7, r5
    xor     r5, r4                          ; hh>>2 ^ hh>>7
    move    r2, r5
    shrq    #3, r5
    move    r3, r6
    shrq    #5, r6
    xor     r6, r5
    or      r5, r4                          ; | hh>>3 ^ tc>>5
    move    r3, r5
    shrq    #3, r5
    xor     r6, r5
    or      r5, r4                          ; | tc>>3 ^ tc>>5
    moveq   #1, r5
    and     r5, r4                          ; x
    move    r16, r6
    and     r5, r6                          ; noise bit
    ; HH: x << 9 | (x ^ n ? $D0 : $34)
    move    r4, r7
    xor     r6, r7
    neg     r7
    movei   #$9C, r8
    and     r8, r7
    movei   #$34, r8
    add     r8, r7
    move    r4, r8
    shlq    #9, r8
    or      r8, r7
    movei   #opl_ridx1, r9
    store   r7, (r9)
    ; SD: (hh >> 8 & 1) << 9 | ((hh >> 8 ^ n) & 1) << 8
    move    r2, r7
    shrq    #8, r7
    and     r5, r7
    move    r7, r8
    shlq    #9, r8
    xor     r6, r7
    shlq    #8, r7
    or      r8, r7
    movei   #opl_ridx2, r9
    store   r7, (r9)
    ; TC: x << 9 | $80
    shlq    #9, r4
    bset    #7, r4
    movei   #opl_ridx3, r9
    store   r4, (r9)
    ; TOM: its own phase, as any operator
    movei   #slots+(14*SLOT_SIZE)+(S_PH*4), r2
    load    (r2), r2
    shrq    #22, r2
    movei   #opl_ridx0, r9
    store   r2, (r9)

; One channel: the modulator (with feedback), then the carrier.  Each
; operator: audible (ATT3 < $C00): phase, wave; silent: phase only, out 0;
; dead (ATT3 = OPL_DEAD): nothing at all.  The audible path is the straight
; one; silent and dead ones jump out of line (.m_sil, .k_sil) and back.  A
; channel whose two operators are dead costs a test.
.ch_loop:
    load    (r15), r14                      ; C_SLOT
    load    (r15+C_CAR), r11
    load    (r14+S_ATT3), r2
    move    r11, r3
    addq    #(S_ATT3*4), r3
    load    (r3), r12                       ; the carrier's ATT3
    movei   #.ch_next, r0
    move    r2, r3
    and     r12, r3
    btst    #12, r3
    jump    NE, (r0)                        ; both dead: nothing
    moveq   #0, r10                         ; (delay slot: the mod's out)
    movei   #.m_sil, r0
    cmp     r26, r2                         ; att3 - $C00: CC = not audible
    jump    CC, (r0)
    nop
    load    (r14+S_PH), r3
    load    (r14+S_INC), r4
    load    (r14+S_OUT), r5
    load    (r14+S_PROUT), r6
    add     r3, r4
    store   r4, (r14+S_PH)
    load    (r14+S_FBSH), r7
    load    (r14+S_FBMASK), r8
    store   r5, (r14+S_PROUT)
    add     r5, r6
    sha     r7, r6
    and     r8, r6                          ; feedback
    shrq    #22, r3
    add     r6, r3                          ; idx
    ; --- the wave (op_wave): idx r3, att3 r2 -> r1 ---
    move    r3, r4
    shrq    #8, r4
    shlq    #30, r4
    shrq    #28, r4
    load    (r14+r4), r5                    ; the quadrant's entry
    move    r3, r6
    xor     r5, r6
    and     r27, r6
    shlq    #2, r6
    add     r29, r6
    load    (r6), r6
    and     r28, r6                         ; log-sine
    move    r5, r7
    and     r26, r7
    add     r2, r6
    add     r7, r6                          ; level
    move    r6, r7
    and     r27, r7
    shlq    #2, r7
    add     r29, r7
    load    (r7), r1
    shrq    #16, r1                         ; exp
    shrq    #8, r6
    sh      r6, r1
    sharq   #31, r5
    xor     r5, r1
    ; ---
    store   r1, (r14+S_OUT)
    move    r1, r10                         ; the modulator's out
.m_done:
    load    (r15+C_MODIN), r9
    move    r11, r14                        ; the carrier
    move    r12, r2
    and     r10, r9                         ; the carrier's modulation
    movei   #.k_sil, r0
    cmp     r26, r2
    jump    CC, (r0)
    nop
    load    (r14+S_PH), r3
    load    (r14+S_INC), r4
    add     r3, r4
    store   r4, (r14+S_PH)
    shrq    #22, r3
    add     r9, r3
    move    r3, r4
    shrq    #8, r4
    shlq    #30, r4
    shrq    #28, r4
    load    (r14+r4), r5                    ; the quadrant's entry
    move    r3, r6
    xor     r5, r6
    and     r27, r6
    shlq    #2, r6
    add     r29, r6
    load    (r6), r6
    and     r28, r6                         ; log-sine
    move    r5, r7
    and     r26, r7
    add     r2, r6
    add     r7, r6                          ; level
    move    r6, r7
    and     r27, r7
    shlq    #2, r7
    add     r29, r7
    load    (r7), r1
    shrq    #16, r1                         ; exp
    shrq    #8, r6
    sh      r6, r1
    sharq   #31, r5
    xor     r5, r1
.k_done:
    load    (r15+C_MODMIX), r2
    load    (r15+C_OSH), r3
    and     r10, r2
    add     r1, r2
    sh      r3, r2
    add     r2, r24
.ch_next:
    movei   #CH_SIZE, r1
    add     r1, r15
    movei   #.ch_loop, r0
    subq    #1, r13
    jump    NE, (r0)
    nop
    movei   #prof_rhy, r0
    jump    T, (r0)
    nop

; Out of line: the not-audible operators.  Silent: out 0, phase; dead:
; untouched.
.m_sil:
    movei   #.m_done, r0
    btst    #12, r2
    jump    NE, (r0)                        ; dead
    nop
    moveq   #0, r1
    store   r1, (r14+S_OUT)
    store   r1, (r14+S_PROUT)
    load    (r14+S_PH), r3
    load    (r14+S_INC), r4
    add     r4, r3
    store   r3, (r14+S_PH)
    jump    T, (r0)
    nop
.k_sil:
    movei   #.k_done, r0
    moveq   #0, r1
    btst    #12, r2
    jump    NE, (r0)                        ; dead
    nop
    load    (r14+S_PH), r3
    load    (r14+S_INC), r4
    add     r4, r3
    store   r3, (r14+S_PH)
    jump    T, (r0)
    nop

prof_rhy:
    ; --- The rhythm block: HH, TOM, SD, TC, each from its index, doubled.
    movei   #opl_rhy, r1
    load    (r1), r1
    movei   #.rhy_done, r0
    btst    #5, r1
    jump    EQ, (r0)
    nop
    movei   #rhy_tab, r15
    moveq   #4, r13
    moveq   #0, r10                         ; the four, summed
.rhy_loop:
    load    (r15), r14                      ; the slot
    load    (r15+1), r3                     ; where its index is
    load    (r14+S_ATT3), r2
    load    (r14+S_PH), r5
    load    (r14+S_INC), r4
    load    (r3), r3
    add     r5, r4
    store   r4, (r14+S_PH)
    movei   #.rhy_silent, r0
    cmp     r26, r2
    jump    CC, (r0)
    nop
    move    r3, r4
    shrq    #8, r4
    shlq    #30, r4
    shrq    #28, r4
    load    (r14+r4), r5
    move    r3, r6
    xor     r5, r6
    and     r27, r6
    shlq    #2, r6
    add     r29, r6
    load    (r6), r6
    and     r28, r6
    move    r5, r7
    and     r26, r7
    add     r2, r6
    add     r7, r6
    move    r6, r7
    and     r27, r7
    shlq    #2, r7
    add     r29, r7
    load    (r7), r1
    shrq    #16, r1
    shrq    #8, r6
    sh      r6, r1
    sharq   #31, r5
    xor     r5, r1
    store   r1, (r14+S_OUT)
    add     r1, r10
    movei   #.rhy_next, r0
    jump    T, (r0)
    nop
.rhy_silent:
    moveq   #0, r1
    store   r1, (r14+S_OUT)
    store   r1, (r14+S_PROUT)
.rhy_next:
    addq    #8, r15
    movei   #.rhy_loop, r0
    subq    #1, r13
    jump    NE, (r0)
    nop
    shlq    #1, r10
    add     r10, r24
.rhy_done:

prof_out:
    ; --- The noise: Nuked OPL3's 23-bit LFSR, one step a sample.
    move    r16, r1
    shrq    #14, r1
    xor     r16, r1
    moveq   #1, r2
    and     r2, r1
    shlq    #22, r1
    shrq    #1, r16
    or      r1, r16

    ; --- Out: sat16(sat16(mix) * 2 + effect << 8)
    sat16s  r24
    shlq    #1, r24
    movei   #sfx_rem, r1
    load    (r1), r2
    movei   #.no_sfx, r0
    cmpq    #0, r2
    jump    EQ, (r0)
    nop
    movei   #sfx_k, r3
    load    (r3), r4
    movei   #sfx_cur, r5
    load    (r5), r6
    cmpq    #0, r4
    jr      NE, .sfx_have
    nop
    ; a new long: four samples, from the cartridge
    movei   #sfx_ptr, r7
    load    (r7), r8
    nop
    load    (r8), r6                        ; EXTERNAL: used at once
    or      r6, r6
    addq    #4, r8
    store   r8, (r7)
    moveq   #4, r4
.sfx_have:
    move    r6, r9
    sharq   #24, r9                         ; the top byte, signed
    shlq    #8, r9                          ; (gain: tools/sr2jag.py SFX_GAIN)
    add     r9, r24
    shlq    #8, r6
    subq    #1, r4
    subq    #1, r2
    store   r6, (r5)
    store   r4, (r3)
    store   r2, (r1)
.no_sfx:
    sat16s  r24

    ; --- Into the ring, then publish the write index.
    move    r23, r1
    and     r19, r1
    shlq    #2, r1
    add     r20, r1
    store   r24, (r1)
    addq    #1, r23
    store   r23, (r22)
dsp_emitted:                                ; (tools/oracle/dsp_check.py)

    ; --- The counters every 64 samples, for the 68000 and the COM8 log.
    movei   #.no_pub, r0
    move    r23, r1
    and     r19, r1
    jump    NE, (r0)
    nop
    movei   #MUS_SEQ, r2                    ; S19: a completed external load
    load    (r2), r2                        ; before the external stores
    or      r2, r2
    load    (r21), r1                       ; rd
    movei   #snd_starve, r3
    load    (r3), r3
    movei   #SND_STARVE, r4
    store   r3, (r4)
    add     r3, r1                          ; rd + starvation = interrupts
    movei   #SND_IRQ, r4
    store   r1, (r4)
    movei   #SND_DONE, r4
    store   r23, (r4)
    movei   #spins, r3
    load    (r3), r3
    movei   #SND_SPIN, r4
    store   r3, (r4)
.no_pub:
    movei   #dsp_idle, r0
    jump    T, (r0)
    nop

dsp_spin:
    ; The ring is full.  The first time, switch the I2S on (Keen Dreams:
    ; there is food for 64 samples, the first interrupt cannot find it
    ; empty).  Every time, count: spins per second = the time left over.
    movei   #spins, r13
    load    (r13), r14
    addq    #1, r14
    store   r14, (r13)
    movei   #i2s_armed, r13
    load    (r13), r14
    movei   #dsp_idle, r0
    cmpq    #0, r14
    jump    NE, (r0)
    nop
    moveq   #1, r14
    store   r14, (r13)
    movei   #D_FLAGS, r13
    movei   #$00004020, r14                 ; REGPAGE + I2S
    store   r14, (r13)
    jump    T, (r0)
    nop

;---------------------------------------------------------------------------
; seq_tick - one MUZAX tick (opl2_model.c seq_tick), link r12.  First the
; 68000's requests: a new stream (MUS_SEQ) or a new effect (SND_SEQ), each
; written by the 68000 with its sequence LAST; then the stream's writes up
; to a wait.
;---------------------------------------------------------------------------
seq_tick:
    movei   #MUS_SEQ, r1
    load    (r1), r2
    or      r2, r2
    movei   #mus_seen, r3
    load    (r3), r4
    movei   #.st_snd, r0
    cmp     r2, r4
    jump    EQ, (r0)
    nop
    store   r2, (r3)
    movei   #MUS_ADR, r1
    load    (r1), r4
    or      r4, r4
    movei   #seq_ptr, r3
    store   r4, (r3)
    movei   #MUS_LOOP, r1
    load    (r1), r4
    or      r4, r4
    movei   #seq_loop, r3
    store   r4, (r3)
    moveq   #0, r4
    movei   #seq_wait, r3
    store   r4, (r3)
.st_snd:
    movei   #SND_SEQ, r1
    load    (r1), r2
    or      r2, r2
    movei   #snd_seen, r3
    load    (r3), r4
    movei   #.st_wait, r0
    cmp     r2, r4
    jump    EQ, (r0)
    nop
    store   r2, (r3)
    movei   #SND_ADR, r1
    load    (r1), r4
    or      r4, r4
    movei   #sfx_ptr, r3
    store   r4, (r3)
    movei   #SND_LEN, r1
    load    (r1), r4
    or      r4, r4
    movei   #sfx_rem, r3
    store   r4, (r3)
    moveq   #0, r4
    movei   #sfx_k, r3
    store   r4, (r3)
.st_wait:
    movei   #seq_wait, r3
    load    (r3), r4
    movei   #.st_run, r0
    cmpq    #0, r4
    jump    EQ, (r0)
    nop
    subq    #1, r4
    store   r4, (r3)
    jump    T, (r12)
    nop
.st_run:
    movei   #seq_ptr, r3
    load    (r3), r5
    movei   #.st_next, r0
    cmpq    #0, r5
    jump    NE, (r0)                        ; no stream yet: nothing
    nop
    jump    T, (r12)
    nop
.st_next:
    ; The entry at r5: the long's high half if r5 is a multiple of 4.
    move    r5, r6
    shrq    #2, r6
    shlq    #2, r6
    load    (r6), r7                        ; EXTERNAL
    or      r7, r7
    btst    #1, r5
    jr      NE, .st_lo
    nop
    shrq    #16, r7
.st_lo:
    shlq    #16, r7
    shrq    #16, r7
    addq    #2, r5
    movei   #$FF00, r6
    movei   #.st_noloop, r0
    cmp     r6, r7
    jump    NE, (r0)
    nop
    movei   #seq_loop, r6                   ; $FF00: on from the loop point
    load    (r6), r5
    movei   #.st_next, r0
    jump    T, (r0)
    nop
.st_noloop:
    move    r7, r6
    shrq    #8, r6                          ; the register
    movei   #.st_write, r0
    cmpq    #0, r6
    jump    NE, (r0)
    nop
    movei   #.st_next, r0                   ; $0000: nothing
    cmpq    #0, r7
    jump    EQ, (r0)
    nop
    subq    #1, r7                          ; a wait of N: N - 1 more ticks
    movei   #seq_wait, r6
    store   r7, (r6)
    movei   #seq_ptr, r6
    store   r5, (r6)
    jump    T, (r12)
    nop
.st_write:
    movei   #seq_ptr, r1
    store   r5, (r1)
    move    r6, r1
    move    r7, r2
    and     r27, r2
    movei   #.st_back, r11
    movei   #opl_write, r0
    jump    T, (r0)
    nop
.st_back:
    movei   #seq_ptr, r3
    load    (r3), r5
    movei   #.st_next, r0
    jump    T, (r0)
    nop

;---------------------------------------------------------------------------
; lfo_step (opl2_model.c), link r12: one tremolo step every 64 chip samples,
; one vibrato step every 16 of those.  Tremolo goes to r25 and to the ATT3
; of every AM slot; vibrato to the increments of every VIB slot.
;---------------------------------------------------------------------------
lfo_step:
    movei   #opl_tpos, r1
    load    (r1), r2
    addq    #1, r2
    movei   #210, r3
    cmp     r3, r2
    jr      NE, .ls_t
    nop
    moveq   #0, r2
.ls_t:
    store   r2, (r1)
    movei   #105, r4
    cmp     r4, r2                          ; tpos - 105: CS below
    jr      CS, .ls_up
    nop
    sub     r2, r3                          ; 210 - tpos
    move    r3, r2
.ls_up:
    movei   #opl_tremsh, r1
    load    (r1), r1
    sh      r1, r2
    move    r2, r25
    ; vibrato every 16th
    movei   #opl_vcnt, r1
    load    (r1), r2
    addq    #1, r2
    moveq   #0, r13                         ; 0: no vibrato step
    movei   #16, r3
    cmp     r3, r2
    jr      NE, .ls_v
    nop
    moveq   #0, r2
    moveq   #1, r13
    movei   #opl_vibpos, r3
    load    (r3), r4
    addq    #1, r4
    moveq   #7, r5
    and     r5, r4
    store   r4, (r3)
.ls_v:
    store   r2, (r1)
    ; every slot: AM -> ATT3; VIB (on a vibrato step) -> increment
    movei   #slots, r14
    moveq   #18, r15
.ls_slot:
    load    (r14+S_AMM), r1
    movei   #.ls_noam, r0
    cmpq    #0, r1
    jump    EQ, (r0)
    nop
    movei   #.ls_noam, r7
    movei   #att3_calc, r0
    jump    T, (r0)
    nop
.ls_noam:
    movei   #.ls_next, r0
    cmpq    #0, r13
    jump    EQ, (r0)
    nop
    load    (r14+S_R20), r1
    btst    #6, r1
    jump    EQ, (r0)
    nop
    ; slot_inc clobbers r13 and r15: keep them
    movei   #lfo_save, r1
    store   r13, (r1)
    addq    #4, r1
    store   r15, (r1)
    movei   #.ls_back, r9
    movei   #slot_inc, r0
    jump    T, (r0)
    nop
.ls_back:
    movei   #lfo_save, r1
    load    (r1), r13
    addq    #4, r1
    load    (r1), r15
.ls_next:
    movei   #SLOT_SIZE, r1
    add     r1, r14
    movei   #.ls_slot, r0
    subq    #1, r15
    jump    NE, (r0)
    nop
    jump    T, (r12)
    nop

;---------------------------------------------------------------------------
; opl_write - a write to the YM3812 (opl2_model.c opl2_write), link r11.
;   r1 register, r2 value.  r13 keeps the value across the helpers.
;---------------------------------------------------------------------------
opl_write:
    move    r2, r13
    move    r1, r3
    shrq    #5, r3                          ; the group: 1 $20, 2 $40, 3 $60,
                                            ; 4 $80, 5 $A0, 6 $C0, 7 $E0
    movei   #.w_low, r0
    cmpq    #0, r3
    jump    EQ, (r0)
    nop
    movei   #.w_ch, r0
    cmpq    #5, r3
    jump    EQ, (r0)
    nop
    cmpq    #6, r3
    jump    EQ, (r0)
    nop
    ; an operator register: the slot from the low 5 bits n, which address
    ; slot (n >> 3) * 6 + (n & 7) when n & 7 < 6 and n < $16
    move    r1, r4
    shlq    #27, r4
    shrq    #27, r4
    move    r4, r5
    shrq    #3, r5                          ; n >> 3
    moveq   #7, r6
    and     r6, r4                          ; n & 7
    movei   #.w_ret, r0
    cmpq    #6, r4
    jump    PL, (r0)                        ; no such slot
    nop
    cmpq    #3, r5
    jump    PL, (r0)
    nop
    move    r5, r6
    shlq    #1, r6
    add     r5, r6
    shlq    #1, r6
    add     r4, r6                          ; the slot's number
    movei   #SLOT_SIZE, r4
    mult    r6, r4
    movei   #slots, r14
    add     r4, r14
    movei   #.w_r40, r0
    cmpq    #2, r3
    jump    EQ, (r0)
    nop
    movei   #.w_r60, r0
    cmpq    #3, r3
    jump    EQ, (r0)
    nop
    movei   #.w_r80, r0
    cmpq    #4, r3
    jump    EQ, (r0)
    nop
    movei   #.w_re0, r0
    cmpq    #7, r3
    jump    EQ, (r0)
    nop
    ; $20: AM VIB EGT KSR MULT
    store   r13, (r14+S_R20)
    move    r13, r1
    shlq    #24, r1
    sharq   #31, r1                         ; AM -> -1 / 0
    store   r1, (r14+S_AMM)
    movei   #.w20_b, r9
    movei   #slot_inc, r0
    jump    T, (r0)
    nop
.w20_b:
    movei   #.w_ret, r9
    movei   #slot_rates, r0
    jump    T, (r0)
    nop
.w_r40:
    store   r13, (r14+S_R40)
    movei   #.w_ret, r9
    movei   #slot_att, r0
    jump    T, (r0)
    nop
.w_r60:
    store   r13, (r14+S_R60)
    movei   #.w_ret, r9
    movei   #slot_rates, r0
    jump    T, (r0)
    nop
.w_r80:
    store   r13, (r14+S_R80)
    movei   #.w_ret, r9
    movei   #slot_rates, r0
    jump    T, (r0)
    nop
.w_re0:
    store   r13, (r14+S_RE0)
    movei   #.w_ret, r9
    movei   #slot_wave, r0
    jump    T, (r0)
    nop
.w_low:
    ; $01: wave select enable; $08: NTS.  $BD lives with the channels.
    movei   #.w_08, r0
    cmpq    #1, r1
    jump    NE, (r0)
    nop
    movei   #$20, r2
    and     r13, r2
    movei   #opl_wse, r3
    store   r2, (r3)
    movei   #.w_ret, r0
    jump    T, (r0)
    nop
.w_08:
    movei   #.w_ret, r0
    cmpq    #8, r1
    jump    NE, (r0)
    nop
    move    r13, r2
    shrq    #6, r2
    moveq   #1, r3
    and     r3, r2
    movei   #opl_nts, r3
    store   r2, (r3)
    jump    T, (r0)
    nop
.w_ch:
    movei   #$BD, r2
    movei   #.w_bd, r0
    cmp     r2, r1
    jump    EQ, (r0)
    nop
    move    r1, r2
    shlq    #28, r2
    shrq    #28, r2                         ; the channel
    movei   #.w_ret, r0
    cmpq    #9, r2
    jump    PL, (r0)                        ; 9..15: none
    nop
    movei   #CH_SIZE, r4
    mult    r2, r4
    movei   #chans, r15
    add     r4, r15
    move    r1, r4
    shrq    #4, r4                          ; $A, $B or $C
    movei   #.w_b0, r0
    cmpq    #$B, r4
    jump    EQ, (r0)
    nop
    movei   #.w_c0, r0
    cmpq    #$C, r4
    jump    EQ, (r0)
    nop
    ; $A0: F-number low
    load    (r15+C_FNUM), r1
    movei   #$300, r2
    and     r2, r1
    or      r13, r1
    store   r1, (r15+C_FNUM)
    movei   #.w_ret, r10
    movei   #ch_freq, r0
    jump    T, (r0)
    nop
.w_b0:
    ; $B0: F-number high, block, key
    load    (r15+C_FNUM), r1
    and     r27, r1
    move    r13, r2
    moveq   #3, r3
    and     r3, r2
    shlq    #8, r2
    or      r2, r1
    store   r1, (r15+C_FNUM)
    move    r13, r2
    shrq    #2, r2
    moveq   #7, r3
    and     r3, r2
    store   r2, (r15+C_BLOCK)
    movei   #.w_b0_k, r10
    movei   #ch_freq, r0
    jump    T, (r0)
    nop
.w_b0_k:
    ; the channel's key on both slots: (key & 2) | (val >> 5 & 1)
    load    (r15), r14
    move    r13, r2
    shrq    #5, r2
    moveq   #1, r3
    and     r3, r2
    movei   #wkey, r3
    store   r2, (r3)
    load    (r14+S_KEY), r1
    moveq   #2, r3
    and     r3, r1
    or      r2, r1
    movei   #.w_b0_k2, r9
    movei   #slot_key, r0
    jump    T, (r0)
    nop
.w_b0_k2:
    load    (r15+C_CAR), r14
    movei   #wkey, r2
    load    (r2), r2
    load    (r14+S_KEY), r1
    moveq   #2, r3
    and     r3, r1
    or      r2, r1
    movei   #.w_ret, r9
    movei   #slot_key, r0
    jump    T, (r0)
    nop
.w_c0:
    move    r13, r1
    shrq    #1, r1
    moveq   #7, r2
    and     r2, r1
    store   r1, (r15+C_FB)
    moveq   #1, r1
    and     r13, r1
    store   r1, (r15+C_CON)
    movei   #.w_ret, r9
    movei   #setup_ch, r0
    jump    T, (r0)
    nop
.w_bd:
    ; $BD: tremolo depth, vibrato depth, rhythm mode, the drums' keys
    move    r13, r1
    shrq    #7, r1
    moveq   #1, r2
    xor     r2, r1
    shlq    #1, r1
    addq    #2, r1
    movei   #opl_tremsh, r2
    store   r1, (r2)
    move    r13, r1
    shrq    #6, r1
    moveq   #1, r2
    and     r2, r1
    xor     r2, r1
    movei   #opl_vibsh, r2
    store   r1, (r2)
    movei   #$3F, r1
    and     r13, r1
    movei   #opl_rhy, r2
    store   r1, (r2)
    movei   #chans+(6*CH_SIZE), r15
    movei   #.w_bd1, r9
    movei   #setup_ch, r0
    jump    T, (r0)
    nop
.w_bd1:
    movei   #chans+(7*CH_SIZE), r15
    movei   #.w_bd2, r9
    movei   #setup_ch, r0
    jump    T, (r0)
    nop
.w_bd2:
    movei   #chans+(8*CH_SIZE), r15
    movei   #.w_bd3, r9
    movei   #setup_ch, r0
    jump    T, (r0)
    nop
.w_bd3:
    ; the drum keys: on = rhy & $20 ? rhy : 0; each drum slot gets
    ; (key & 1) | (on & its bit ? 2 : 0), in Nuked's order
    movei   #opl_rhy, r1
    load    (r1), r1
    btst    #5, r1
    jr      NE, .w_bdon
    nop
    moveq   #0, r1
.w_bdon:
    movei   #wkey, r2
    store   r1, (r2)
    movei   #drum_tab, r2
    movei   #wptr, r3
    store   r2, (r3)
.w_bdk:
    movei   #wptr, r3
    load    (r3), r2
    load    (r2), r14                       ; the slot, 0 = end
    movei   #.w_ret, r0
    cmpq    #0, r14
    jump    EQ, (r0)
    nop
    addq    #4, r2
    load    (r2), r4                        ; its bit
    addq    #4, r2
    store   r2, (r3)
    movei   #wkey, r1
    load    (r1), r1
    moveq   #0, r5
    and     r4, r1
    jr      EQ, .w_bdk0
    nop
    moveq   #2, r5
.w_bdk0:
    load    (r14+S_KEY), r1
    moveq   #1, r2
    and     r2, r1
    or      r5, r1
    movei   #.w_bdk, r9
    movei   #slot_key, r0
    jump    T, (r0)
    nop
.w_ret:
    jump    T, (r11)
    nop

;---------------------------------------------------------------------------
; ch_freq - after A0/B0: both slots' increment, KSL and rates.  r15 =
; channel, link r10.
;---------------------------------------------------------------------------
ch_freq:
    load    (r15), r14
    movei   #.cf1, r9
    movei   #slot_inc, r0
    jump    T, (r0)
    nop
.cf1:
    movei   #.cf2, r9
    movei   #slot_att, r0
    jump    T, (r0)
    nop
.cf2:
    movei   #.cf3, r9
    movei   #slot_rates, r0
    jump    T, (r0)
    nop
.cf3:
    load    (r15+C_CAR), r14
    movei   #.cf4, r9
    movei   #slot_inc, r0
    jump    T, (r0)
    nop
.cf4:
    movei   #.cf5, r9
    movei   #slot_att, r0
    jump    T, (r0)
    nop
.cf5:
    movei   #.cf6, r9
    movei   #slot_rates, r0
    jump    T, (r0)
    nop
.cf6:
    jump    T, (r10)
    nop

;---------------------------------------------------------------------------
; slot_inc - the phase increment (opl2_model.c slot_inc).  r14 slot, link r9;
; sets r15 to its channel.
;---------------------------------------------------------------------------
slot_inc:
    load    (r14+S_CH), r15
    load    (r15+C_FNUM), r1
    load    (r15+C_BLOCK), r2
    load    (r14+S_R20), r3
    movei   #.si_novib, r0
    btst    #6, r3
    jump    EQ, (r0)
    nop
    ; vibrato: range = (f >> 7) & 7, shaped by the position
    move    r1, r4
    shrq    #7, r4
    moveq   #7, r5
    and     r5, r4
    movei   #opl_vibpos, r5
    load    (r5), r5
    moveq   #3, r6
    and     r5, r6
    jr      NE, .si_v1
    nop
    moveq   #0, r4
.si_v1:
    btst    #0, r5
    jr      EQ, .si_v2
    nop
    shrq    #1, r4
.si_v2:
    movei   #opl_vibsh, r6
    load    (r6), r6
    sh      r6, r4
    btst    #2, r5
    jr      EQ, .si_v3
    nop
    neg     r4
.si_v3:
    add     r4, r1
    shlq    #16, r1
    shrq    #16, r1
.si_novib:
    neg     r2
    sh      r2, r1                          ; f << block
    moveq   #15, r4
    and     r3, r4
    shlq    #2, r4
    movei   #T_MULT, r5
    add     r5, r4
    load    (r4), r4                        ; MULT, EXTERNAL
    or      r4, r4
    ; the low 32 bits of r1 * r4, from 16 x 16 mults
    move    r1, r5
    mult    r4, r5                          ; al * bl
    move    r4, r6
    shrq    #16, r6
    mult    r1, r6                          ; bh * al
    move    r1, r2
    shrq    #16, r2
    mult    r4, r2                          ; bl * ah
    add     r2, r6
    shlq    #16, r6
    add     r6, r5
    store   r5, (r14+S_INC)
    jump    T, (r9)
    nop

;---------------------------------------------------------------------------
; slot_att - TL and KSL (opl2_model.c slot_att), then ATT3.  r14, link r9.
;---------------------------------------------------------------------------
slot_att:
    load    (r14+S_CH), r15
    load    (r15+C_FNUM), r1
    load    (r15+C_BLOCK), r2
    shrq    #6, r1
    shlq    #2, r1
    movei   #T_KSL, r3
    add     r3, r1
    load    (r1), r1                        ; KSL[fnum >> 6], EXTERNAL
    or      r1, r1
    moveq   #8, r3
    sub     r2, r3
    shlq    #5, r3
    sub     r3, r1
    jr      PL, .sa_pos
    nop
    moveq   #0, r1
.sa_pos:
    load    (r14+S_R40), r2
    move    r2, r3
    shrq    #6, r3                          ; KSL setting
    shlq    #2, r3
    movei   #kslshift, r4
    add     r4, r3
    load    (r3), r3
    sh      r3, r1
    movei   #63, r3
    and     r3, r2
    shlq    #2, r2                          ; TL * 4
    add     r2, r1
    store   r1, (r14+S_ATTS)
    movei   #.sa_ret, r7
    movei   #att3_calc, r0
    jump    T, (r0)
    nop
.sa_ret:
    jump    T, (r9)
    nop

;---------------------------------------------------------------------------
; slot_rates - the envelope's rates with key scaling (opl2_model.c
; slot_rates), then eg_refresh.  r14, link r9.
;---------------------------------------------------------------------------
slot_rates:
    load    (r14+S_CH), r15
    load    (r15+C_BLOCK), r1
    load    (r15+C_FNUM), r2
    movei   #opl_nts, r3
    load    (r3), r3
    moveq   #9, r4
    sub     r3, r4
    sh      r4, r2
    moveq   #1, r4
    and     r4, r2
    shlq    #1, r1
    or      r2, r1                          ; ksv
    load    (r14+S_R20), r2
    shrq    #4, r2
    moveq   #1, r4
    and     r4, r2
    xor     r4, r2
    shlq    #1, r2
    sh      r2, r1                          ; ks = ksv >> ((ksr ^ 1) << 1)
    movei   #rate_ks, r2
    store   r1, (r2)
    ; AR, DR from $60; RR from $80
    load    (r14+S_R60), r2
    move    r2, r3
    shrq    #4, r3
    movei   #T_AINC, r4
    movei   #.sr_a, r6
    movei   #rate_lookup, r0
    jump    T, (r0)
    nop
.sr_a:
    store   r5, (r14+S_AINC)
    load    (r14+S_R60), r3
    moveq   #15, r2
    and     r2, r3
    movei   #T_DINC, r4
    movei   #.sr_d, r6
    movei   #rate_lookup, r0
    jump    T, (r0)
    nop
.sr_d:
    store   r5, (r14+S_DINCD)
    load    (r14+S_R80), r3
    moveq   #15, r2
    and     r2, r3
    movei   #T_DINC, r4
    movei   #.sr_r, r6
    movei   #rate_lookup, r0
    jump    T, (r0)
    nop
.sr_r:
    store   r5, (r14+S_DINCR)
    ; SL: 15 means 31; the limit min(SL << 4, $1F8) << 16
    load    (r14+S_R80), r3
    shrq    #4, r3
    cmpq    #15, r3
    jr      NE, .sr_sl
    nop
    moveq   #31, r3
.sr_sl:
    shlq    #4, r3
    movei   #$1F8, r2
    cmp     r2, r3
    jr      CS, .sr_sl2
    nop
    move    r2, r3
.sr_sl2:
    shlq    #16, r3
    store   r3, (r14+S_SLIM)
    movei   #.sr_ret, r8
    movei   #eg_refresh, r0
    jump    T, (r0)
    nop
.sr_ret:
    jump    T, (r9)
    nop

; rate_lookup: r3 = the register's rate (0..15), r4 = the table -> r5 =
; r3 ? table[min(ks + 4 r3, 63)] : 0.  Link r6 (NOT r12: slot_rates runs
; under seq_tick, whose return is in r12); r2 scratch.
rate_lookup:
    moveq   #0, r5
    movei   #.rl_ret, r0
    cmpq    #0, r3
    jump    EQ, (r0)
    nop
    shlq    #2, r3
    movei   #rate_ks, r2
    load    (r2), r2
    add     r2, r3
    movei   #63, r2
    cmp     r2, r3
    jr      CS, .rl_ok
    nop
    move    r2, r3
.rl_ok:
    shlq    #2, r3
    add     r4, r3
    load    (r3), r5                        ; EXTERNAL (opl2_rom.inc)
    or      r5, r5
.rl_ret:
    jump    T, (r6)
    nop

;---------------------------------------------------------------------------
; eg_refresh (opl2_model.c), link r8: the state's step and limit, every due
; transition at once, then ATT3.  r14.
;---------------------------------------------------------------------------
eg_refresh:
    load    (r14+S_EGST), r1
    movei   #.er_notatk, r0
    cmpq    #EG_ATTACK, r1
    jump    NE, (r0)
    nop
    load    (r14+S_AINC), r2
    store   r2, (r14+S_EGINC)
    load    (r14+S_ENV), r3
    movei   #.er_done, r0
    cmpq    #0, r3
    jump    NE, (r0)                        ; at 0 the attack is over
    nop
    moveq   #EG_DECAY, r1
    store   r1, (r14+S_EGST)
.er_notatk:
    movei   #ENV_OFF, r3                    ; the limit of SUSTAIN / RELEASE
    load    (r14+S_DINCR), r2
    cmpq    #EG_DECAY, r1
    jr      NE, .er_nd
    nop
    load    (r14+S_DINCD), r2
    load    (r14+S_SLIM), r3
.er_nd:
    cmpq    #EG_SUSTAIN, r1
    jr      NE, .er_ns
    nop
    load    (r14+S_R20), r4
    btst    #5, r4                          ; EG-type: hold
    jr      EQ, .er_ns
    nop
    moveq   #0, r2
.er_ns:
    store   r2, (r14+S_EGINC)
    store   r3, (r14+S_EGLIM)
    load    (r14+S_ENV), r4
    movei   #.er_done, r0
    cmp     r3, r4                          ; env - lim: CS below
    jump    CS, (r0)
    nop
    movei   #.er_notatk, r0
    cmpq    #EG_DECAY, r1
    jr      NE, .er_off
    nop
    moveq   #EG_SUSTAIN, r1
    store   r1, (r14+S_EGST)
    jump    T, (r0)
    nop
.er_off:
    movei   #ENV_MAX, r1
    store   r1, (r14+S_ENV)
    moveq   #0, r1
    store   r1, (r14+S_EGINC)
    movei   #LIM_NONE, r1
    store   r1, (r14+S_EGLIM)
.er_done:
    movei   #.er_ret, r7
    movei   #att3_calc, r0
    jump    T, (r0)
    nop
.er_ret:
    jump    T, (r8)
    nop

; att3_calc: ATT3 = min((ENV >> 16) + ATTS + (tremolo & AMM), 511) << 3.
; r14, link r7, r1-r3.
att3_calc:
    load    (r14+S_ENV), r1
    movei   #ENV_MAX, r2
    cmp     r2, r1
    jr      NE, .ac_live
    nop
    movei   #OPL_DEAD, r1
    store   r1, (r14+S_ATT3)
    jump    T, (r7)
    nop
.ac_live:
    load    (r14+S_ATTS), r2
    load    (r14+S_AMM), r3
    shrq    #16, r1
    and     r25, r3
    add     r2, r1
    movei   #511, r2
    add     r3, r1
    cmp     r2, r1
    jr      CS, .ac_ok
    nop
    move    r2, r1
.ac_ok:
    shlq    #3, r1
    store   r1, (r14+S_ATT3)
    jump    T, (r7)
    nop

;---------------------------------------------------------------------------
; slot_key - r14 slot, r1 the new key (opl2_model.c slot_key), link r9.
;---------------------------------------------------------------------------
slot_key:
    load    (r14+S_KEY), r2
    store   r1, (r14+S_KEY)
    movei   #.sk_on, r0
    cmpq    #0, r2
    jump    EQ, (r0)
    nop
    ; it was on: off now?
    movei   #.sk_ret, r0
    cmpq    #0, r1
    jump    NE, (r0)
    nop
    moveq   #EG_RELEASE, r1
    store   r1, (r14+S_EGST)
    movei   #.sk_ret, r8
    movei   #eg_refresh, r0
    jump    T, (r0)
    nop
.sk_on:
    movei   #.sk_ret, r0
    cmpq    #0, r1
    jump    EQ, (r0)
    nop
    moveq   #0, r1
    store   r1, (r14+S_PH)
    store   r1, (r14+S_OUT)
    store   r1, (r14+S_PROUT)
    store   r1, (r14+S_EGST)                ; EG_ATTACK
    movei   #.sk_ret, r8
    movei   #eg_refresh, r0
    jump    T, (r0)
    nop
.sk_ret:
    jump    T, (r9)
    nop

;---------------------------------------------------------------------------
; slot_wave - the four quadrant entries (opl2_model.c slot_wave).  r14, r9.
;---------------------------------------------------------------------------
slot_wave:
    load    (r14+S_RE0), r1
    moveq   #3, r2
    and     r2, r1
    movei   #opl_wse, r2
    load    (r2), r2
    cmpq    #0, r2
    jr      NE, .sw_en
    nop
    moveq   #0, r1                          ; wave select off: sine
.sw_en:
    shlq    #4, r1
    movei   #qtab, r2
    add     r2, r1
    load    (r1), r2
    store   r2, (r14)
    addq    #4, r1
    load    (r1), r2
    store   r2, (r14+1)
    addq    #4, r1
    load    (r1), r2
    store   r2, (r14+2)
    addq    #4, r1
    load    (r1), r2
    store   r2, (r14+3)
    jump    T, (r9)
    nop

;---------------------------------------------------------------------------
; setup_ch - how channel r15 modulates and mixes (opl2_model.c setup_ch).
; link r9.
;---------------------------------------------------------------------------
setup_ch:
    load    (r15), r14
    load    (r15+C_FB), r1
    moveq   #0, r2                          ; FBMASK
    moveq   #0, r3                          ; FBSH
    cmpq    #0, r1
    jr      EQ, .sc_nofb
    nop
    subq    #1, r2
    moveq   #9, r3
    sub     r1, r3
.sc_nofb:
    store   r2, (r14+S_FBMASK)
    store   r3, (r14+S_FBSH)
    load    (r15+C_CON), r1
    moveq   #0, r2                          ; MODIN = con ? 0 : -1
    subq    #1, r2
    moveq   #0, r3                          ; MODMIX = con ? -1 : 0
    cmpq    #0, r1
    jr      EQ, .sc_c0
    nop
    moveq   #0, r2
    moveq   #0, r3
    subq    #1, r3
.sc_c0:
    store   r2, (r15+C_MODIN)
    moveq   #0, r4                          ; OSH
    ; BD in rhythm mode: the carrier only, doubled
    movei   #opl_rhy, r1
    load    (r1), r1
    btst    #5, r1
    jr      EQ, .sc_st
    nop
    movei   #chans+(6*CH_SIZE), r1
    cmp     r1, r15
    jr      NE, .sc_st
    nop
    moveq   #0, r3
    moveq   #0, r4
    subq    #1, r4
.sc_st:
    store   r3, (r15+C_MODMIX)
    store   r4, (r15+C_OSH)
    jump    T, (r9)
    nop

;---------------------------------------------------------------------------
; Data.
;---------------------------------------------------------------------------
    .long
    .include "opl2.inc"                     ; tools/opl2.py: tables + EQUs

kslshift:
    dc.l    8, 1, 2, 0
qtab:                                       ; the waves' quadrants (slot_wave)
    dc.l    0, $FF, $80000000, $800000FF
    dc.l    0, $FF, $C00, $CFF
    dc.l    0, $FF, 0, $FF
    dc.l    0, $C00, 0, $C00
ch_mod:                                     ; the nine modulators
    dc.l    slots+(0*SLOT_SIZE), slots+(1*SLOT_SIZE), slots+(2*SLOT_SIZE)
    dc.l    slots+(6*SLOT_SIZE), slots+(7*SLOT_SIZE), slots+(8*SLOT_SIZE)
    dc.l    slots+(12*SLOT_SIZE), slots+(13*SLOT_SIZE), slots+(14*SLOT_SIZE)
slot_ch:                                    ; slot n's channel record
    dc.l    chans+(0*CH_SIZE), chans+(1*CH_SIZE), chans+(2*CH_SIZE)
    dc.l    chans+(0*CH_SIZE), chans+(1*CH_SIZE), chans+(2*CH_SIZE)
    dc.l    chans+(3*CH_SIZE), chans+(4*CH_SIZE), chans+(5*CH_SIZE)
    dc.l    chans+(3*CH_SIZE), chans+(4*CH_SIZE), chans+(5*CH_SIZE)
    dc.l    chans+(6*CH_SIZE), chans+(7*CH_SIZE), chans+(8*CH_SIZE)
    dc.l    chans+(6*CH_SIZE), chans+(7*CH_SIZE), chans+(8*CH_SIZE)
drum_tab:                                   ; slot, its bit in $BD (Nuked's order)
    dc.l    slots+(13*SLOT_SIZE), $01       ; HH
    dc.l    slots+(17*SLOT_SIZE), $02       ; TC
    dc.l    slots+(14*SLOT_SIZE), $04       ; TOM
    dc.l    slots+(16*SLOT_SIZE), $08       ; SD
    dc.l    slots+(12*SLOT_SIZE), $10       ; BD
    dc.l    slots+(15*SLOT_SIZE), $10
    dc.l    0
rhy_tab:                                    ; the rhythm block: slot, its index
    dc.l    slots+(13*SLOT_SIZE), opl_ridx1 ; HH
    dc.l    slots+(14*SLOT_SIZE), opl_ridx0 ; TOM (its own phase)
    dc.l    slots+(16*SLOT_SIZE), opl_ridx2 ; SD
    dc.l    slots+(17*SLOT_SIZE), opl_ridx3 ; TC

; Variables (zero at power-up: the 68000 clears all 8 KB).
opl_wse:        dc.l    0
opl_nts:        dc.l    0
opl_rhy:        dc.l    0
opl_tremsh:     dc.l    4
opl_vibsh:      dc.l    1
opl_tpos:       dc.l    0
opl_vcnt:       dc.l    0
opl_vibpos:     dc.l    0
opl_ridx0:      dc.l    0
opl_ridx1:      dc.l    0
opl_ridx2:      dc.l    0
opl_ridx3:      dc.l    0
seq_ptr:        dc.l    0
seq_loop:       dc.l    0
seq_wait:       dc.l    0
mus_seen:       dc.l    0
snd_seen:       dc.l    0
sfx_ptr:        dc.l    0
sfx_rem:        dc.l    0
sfx_cur:        dc.l    0
sfx_k:          dc.l    0
spins:          dc.l    0
rate_ks:        dc.l    0
wkey:           dc.l    0
wptr:           dc.l    0
lfo_save:       dc.l    0, 0

snd_buf:
    dcb.l   DSP_BUF_LEN, 0
chans:
    dcb.l   (9*CH_SIZE)/4, 0
slots:
    dcb.l   (18*SLOT_SIZE)/4, 0
dsp_ram_end:

    .phrase
    .68000
DspAudioRomEnd:
