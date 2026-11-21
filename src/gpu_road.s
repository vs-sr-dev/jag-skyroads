;===========================================================================
; The road rasteriser, on TOM's GPU.
;
; Two launches per rendered frame (csrc/road.h): the first copies the
; pristine backdrop into the back frame buffer and fills the records drawn
; before the ship; the 68000 then draws the ship and its shadow; the second
; fills the records drawn after it.  Each launch walks its command list and
; stops itself.
; The PC executes the same list with tools/oracle/raster.h, and that path is
; pixel-identical to skyroads-port's renderer over the whole demo.
;
; Rules this code follows, each paid for in an earlier project:
;   - external memory is READ only as aligned longs (trekg is laid out for
;     it); bytes are only STORED, which JagLBA verified on hardware;
;   - jump targets live in registers loaded once, BEFORE any compare:
;     movei changes the flags;
;   - no signed conditions (count down to zero, test EQ/NE);
;   - every jump has its delay-slot nop;
;   - the end is JagLBA's epilogue: barrier, DONE, barrier, self-stop.  The
;     68000's writes are posted and so are the GPU's: without the reads, DONE
;     or the last pixels can still be in flight when the other side looks.
;
; Registers:
;   r1  command cursor        r2  current base
;   r3  a / span cursor       r4  b = fill | half<<8 / copy source
;   r5  row offset            r6  lines / longs left
;   r7  x                     r8  scratch
;   r9  span length           r10 destination
;   r19 half                  r20 320
;   r21 $FFFF                 r22 longs in the view
;   r11 copy destination      r12-r18 jump targets
;===========================================================================

    .68000
; Once at boot, before any launch.  The count is rounded UP: the last
; instruction is the halt loop's delay-slot nop, and losing it makes the GPU
; run stale code after DONE (JagLBA).
InitGpu:
    movem.l d0/a0-a1, -(sp)
    move.l  #0, G_CTRL
    lea     GpuRom, a0
    movea.l #G_RAM, a1
    move.w  #(((GpuRomEnd-GpuRom)+3)/4)-1, d0
.ig_copy:
    move.l  (a0)+, (a1)+
    dbra    d0, .ig_copy
    movem.l (sp)+, d0/a0-a1
    rts

GpuRom:
    .gpu
    .org    G_RAM

GpuRoad:
    movei   #GPU_CMD, r0
    load    (r0), r1
    movei   #320, r20
    movei   #$FFFF, r21
    movei   #(VIEW0_SIZE/4), r22
    moveq   #0, r2
    movei   #gr_next, r12
    movei   #gr_done, r13
    movei   #gr_base, r14
    movei   #gr_copy, r15
    movei   #gr_line, r16
    movei   #gr_fill, r17
    movei   #gr_cploop, r18

gr_next:
    load    (r1), r3
    addq    #4, r1
    load    (r1), r4
    addq    #4, r1
    cmpq    #0, r3
    jump    EQ, (r13)
    nop
    cmpq    #1, r3
    jump    EQ, (r14)
    nop
    cmpq    #4, r3
    jump    EQ, (r15)
    nop

; A record: r3 = its span block {row, lines, (x0<<16|len)...}.
    move    r4, r19
    shrq    #8, r19
    load    (r3), r5
    addq    #4, r3
    load    (r3), r6
    addq    #4, r3
    cmpq    #0, r6
    jump    EQ, (r12)
    nop

gr_line:
    load    (r3), r7
    addq    #4, r3
    move    r7, r9
    and     r21, r9
    shrq    #16, r7
    cmpq    #0, r19
    jr      EQ, gr_left
    nop
; The right half mirrors the left around x = 160: x = 320 - x0 - len.
    move    r20, r8
    sub     r7, r8
    sub     r9, r8
    move    r8, r7
gr_left:
    move    r2, r10
    add     r5, r10
    add     r7, r10
    add     r20, r5
    cmpq    #0, r9
    jr      EQ, gr_nofill
    nop
gr_fill:
    storeb  r4, (r10)
    addq    #1, r10
    subq    #1, r9
    jump    NE, (r17)
    nop
gr_nofill:
    subq    #1, r6
    jump    NE, (r16)
    nop
    jump    T, (r12)
    nop

gr_base:
    move    r4, r2
    jump    T, (r12)
    nop

; The pristine screen, long by long: source and destination are both
; phrase-aligned DRAM buffers (tools/memory_map.py).
gr_copy:
    move    r22, r6
    move    r2, r11
gr_cploop:
    load    (r4), r7
    addq    #4, r4
    store   r7, (r11)
    addq    #4, r11
    subq    #1, r6
    jump    NE, (r18)
    nop
    jump    T, (r12)
    nop

; JagLBA's epilogue (GpuCommon_Done), verified on hardware.  The barrier's
; register (r30) is never reused for the value: the load lands late.
gr_done:
    movei   #GPU_DONE, r0
    load    (r0), r30
    nop
    nop
    nop
    nop
    nop
    nop
    moveq   #1, r19
    store   r19, (r0)
    load    (r0), r30
    nop
    nop
    nop
    nop
    nop
    nop
    movei   #G_CTRL, r0
    moveq   #0, r19
    store   r19, (r0)
    nop
    nop
    nop
    nop
    nop
    nop
gr_halt:
    movei   #gr_halt, r0
    jump    (r0)
    nop

    .68000
GpuRomEnd:
