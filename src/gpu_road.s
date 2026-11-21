;===========================================================================
; The road on TOM's GPU: the composers and the span rasteriser.
;
; Two launches per rendered frame (csrc/road.h, "THE FRAME"):
;   GPU_CMD = 0   the COMPOSERS (gc_*) read the frame's input block (COMP_IN,
;                 which the 68000 writes: csrc/road.c road_gpu_input) and the
;                 phase object in trekg, write the whole command list to
;                 CMD_BUF, leave the second list's address in GPU_CMD, wait
;                 for the Blitter's viewport copy, then fill the first list;
;   GPU_CMD = L   fill the list at L (the second launch, after the 68000 has
;                 drawn the ship and its shadow).
; Each launch stops itself.  The composers transcribe road_compose
; (csrc/road.c) call for call; the PC runs THIS binary in an interpreter and
; compares its lists with road_compose's (tools/oracle/gpu_check.py), and
; the 68000 can do the same on the hardware (keypad 9, csrc/game_main.c).
; The PC executes the lists with tools/oracle/raster.h, and that path is
; pixel-identical to skyroads-port's renderer over the whole demo.
;
; Rules this code follows, each paid for in an earlier project:
;   - external memory is READ only as aligned longs (trekg and COMP_IN are
;     laid out for it); bytes are only STORED, which JagLBA verified on
;     hardware;
;   - jump targets live in registers loaded BEFORE any compare: movei
;     changes the flags (observed on hardware, against the TRM);
;   - only Z and N are tested, never the signed combinations: the values
;     compared are small, so "x < n" is MI after cmpq #n, x;
;   - subroutines as JagLBA: movei #return, link + jump; back with
;     jump (link);
;   - every jump has its delay-slot nop;
;   - no indexed loads, no mult;
;   - the end is JagLBA's epilogue: barrier, DONE, barrier, self-stop.  The
;     68000's writes are posted and so are the GPU's: without the reads, DONE
;     or the last pixels can still be in flight when the other side looks.
;
; Rasteriser registers (set up at gr_start):
;   r1  command cursor        r2  current base
;   r3  a / span cursor       r4  b = fill | half<<8 / copy source
;   r5  row offset            r6  lines / longs left
;   r7  x                     r8  scratch
;   r9  span length           r10 destination
;   r19 half                  r20 320
;   r21 $FFFF                 r22 longs in the view
;   r11 copy destination      r12-r18, r25 jump targets
;   r23 the fill byte x 4     r24 scratch
; The composers' registers are listed at gc_compose's block.
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
    movei   #gc_compose, r2
    cmpq    #0, r1
    jump    EQ, (r2)
    nop
gr_start:
    movei   #320, r20
    movei   #$FFFF, r21
    movei   #(VIEW0_SIZE/4), r22
    moveq   #0, r2
    movei   #gr_next, r12
    movei   #gr_done, r13
    movei   #gr_base, r14
    movei   #gr_copy, r15
    movei   #gr_line, r16
    movei   #gr_head, r17
    movei   #gr_cploop, r18
    movei   #gr_nofill, r25

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
    move    r4, r23
    shlq    #24, r23
    move    r23, r24
    shrq    #8, r24
    or      r24, r23
    move    r23, r24
    shrq    #16, r24
    or      r24, r23                    ; the fill byte, four times
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
    jump    EQ, (r25)
    nop
; A span: bytes up to a long boundary, then longs, then the last bytes.
; Byte by byte it cost the GPU 100-250 half-lines a launch on the near road
; (M3, hardware); a long store is one bus write for four pixels.
gr_head:
    moveq   #3, r24
    and     r10, r24
    jr      EQ, gr_body
    nop
    storeb  r4, (r10)
    addq    #1, r10
    subq    #1, r9
    jump    NE, (r17)
    nop
    jump    T, (r25)
    nop
gr_body:
    move    r9, r24
    shrq    #2, r24
    jr      EQ, gr_tail
    nop
gr_bodyloop:
    store   r23, (r10)
    addq    #4, r10
    subq    #1, r24
    jr      NE, gr_bodyloop
    nop
gr_tail:
    moveq   #3, r24
    and     r24, r9
    jr      EQ, gr_nofill
    nop
gr_tailloop:
    storeb  r4, (r10)
    addq    #1, r10
    subq    #1, r9
    jr      NE, gr_tailloop
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

;---------------------------------------------------------------------------
; THE COMPOSERS: csrc/road.c road_compose, transcribed.
;
;   r0  scratch / jump target       r16 override colour (0 = the record's)
;   r1  output cursor (CMD_BUF)     r17 gc_fill's link
;   r2  record table                r18 gc_floor's link
;   r3  directory                   r19 window cell of the tile
;   r4  record cursor x 4 (k->cur)  r20 column step: +4 left half, -4 right
;   r5  tile                        r21 columns left in the half
;   r6  nearer (grid row - 1)       r22 halves left
;   r7  inner (the next column      r23 pass table cursor
;       toward the centre)          r24 floor colour (tile & 15)
;   r8  half << 8                   r25 window row of the pass
;   r10 TREKG_BUF                   r26 directory row of the pass
;   r11 $FFFFFF                     r27 gc_fill
;   r12 the half's colour quads     r28 gc_floor
;   r13 15                          r29 gc_next_tile
;   r15 directory cell of the tile  r30, r31 scratch
;       (dir + 4 * (dirrow*24 + column*6))
;---------------------------------------------------------------------------

; fill(override): one record, the cursor advances (gc_fill).
    .macro  FILL ov
    moveq   #\ov, r16
    movei   #.\~, r17
    jump    T, (r27)
    nop
.\~:
    .endm
; the same, with an override moveq cannot carry (0x41, 0x43)
    .macro  FILLI ov
    movei   #\ov, r16
    movei   #.\~, r17
    jump    T, (r27)
    nop
.\~:
    .endm
; the record cursor to kind k of this tile's directory cell
    .macro  SEEK k
    move    r15, r0
    .if \k
    addq    #(4*\k), r0
    .endif
    load    (r0), r4
    shlq    #2, r4
    .endm
    .macro  SKIP
    addq    #4, r4
    .endm
    .macro  FLOOR
    movei   #.\~, r18
    jump    T, (r28)
    nop
.\~:
    .endm
; r0 = the shape (bits 8..11) of the cell in register r
    .macro  SHAPE r
    move    \r, r0
    shrq    #8, r0
    and     r13, r0
    .endm
; r16 = blockcolor(tile): (tile >> 4) & 15, or 0x3d if that is 0
    .macro  BLOCKCOLOR
    move    r5, r16
    shrq    #4, r16
    and     r13, r16
    jr      NE, .\~
    nop
    moveq   #30, r16
    addq    #31, r16
.\~:
    .endm

    .long
; The passes, (directory row x 96, window row x 32), in road_compose's order:
; directory rows 0..6 on grid rows base+7 .. base+1, the ship row's pre-ship
; set (11), the split, its post-ship set (12), then rows 8 and 9 (grid rows
; base, base-1).  Window row w is grid row base - 3 + w.
; -1 = the split, -2 = the end.
gc_passes:
    dc.l    0*96, 10*32
    dc.l    1*96, 9*32
    dc.l    2*96, 8*32
    dc.l    3*96, 7*32
    dc.l    4*96, 6*32
    dc.l    5*96, 5*32
    dc.l    6*96, 4*32
    dc.l    11*96, 3*32
    dc.l    -1, 0
    dc.l    12*96, 3*32
    dc.l    8*96, 2*32
    dc.l    9*96, 1*32
    dc.l    -2, 0
; The composers by shape (tile bits 8..11); 6..15 draw nothing.
gc_shapes:
    dc.l    gc_c_floor, gc_c_tunnel, gc_c_lowblock, gc_c_tun_low
    dc.l    gc_c_highblock, gc_c_tun_high
    dc.l    gc_next_tile, gc_next_tile, gc_next_tile, gc_next_tile
    dc.l    gc_next_tile, gc_next_tile, gc_next_tile, gc_next_tile
    dc.l    gc_next_tile, gc_next_tile

gc_compose:
    movei   #COMP_POS, r0
    load    (r0), r31                   ; pos
    movei   #TREKG_BUF, r10
    moveq   #7, r0
    and     r31, r0
    shlq    #2, r0
    add     r10, r0
    load    (r0), r2
    add     r10, r2                     ; the phase object
    move    r2, r3
    addq    #8, r3                      ; its directory
    move    r2, r0
    addq    #4, r0
    load    (r0), r0
    add     r0, r2                      ; its record table
    movei   #$FFFFFF, r11
    moveq   #15, r13
    movei   #gc_fill, r27
    movei   #gc_floor, r28
    movei   #gc_next_tile, r29
    movei   #gc_passes, r23
    moveq   #0, r4
    movei   #CMD_BUF, r1
    moveq   #1, r0                      ; ROAD_OP_BASE
    store   r0, (r1)
    addq    #4, r1
    movei   #COMP_BASE, r0
    load    (r0), r0
    store   r0, (r1)
    addq    #4, r1

gc_pass:
    load    (r23), r26
    addq    #4, r23
    load    (r23), r25
    addq    #4, r23
    movei   #gc_split, r0
    cmpq    #-1, r26
    jump    EQ, (r0)
    nop
    movei   #gc_end, r0
    cmpq    #-2, r26
    jump    EQ, (r0)
    nop
    add     r3, r26                     ; directory row
    movei   #COMP_WIN, r0
    add     r0, r25                     ; window row
; the left half: columns 0..3, inner = the next one
    moveq   #2, r22
    moveq   #0, r8
    movei   #COMP_QUAD0, r12
    moveq   #4, r20
    move    r25, r19
gc_half:
    moveq   #4, r21
    move    r26, r15
gc_tile:
    load    (r19), r5                   ; tile
    move    r19, r0
    add     r20, r0
    load    (r0), r7                    ; inner
    move    r19, r0
    subq    #32, r0
    load    (r0), r6                    ; nearer
    SHAPE   r5
    shlq    #2, r0
    movei   #gc_shapes, r30
    add     r30, r0
    load    (r0), r0
    jump    T, (r0)
    nop
gc_next_tile:
    add     r20, r19
    addq    #24, r15
    movei   #gc_tile, r30
    subq    #1, r21
    jump    NE, (r30)
    nop
    movei   #gc_pass, r30
    subq    #1, r22
    jump    EQ, (r30)
    nop
; the right half: columns 6..3, inner = the previous one
    movei   #$100, r8
    movei   #COMP_QUAD1, r12
    moveq   #4, r20
    neg     r20
    move    r25, r19
    addq    #24, r19
    movei   #gc_half, r30
    jump    T, (r30)
    nop

; The ship goes here: list 1 ends, list 2 starts at the same base.
gc_split:
    moveq   #0, r0                      ; ROAD_OP_END
    store   r0, (r1)
    addq    #4, r1
    store   r0, (r1)
    addq    #4, r1
    movei   #GPU_CMD, r0
    store   r1, (r0)                    ; the second launch's list
    moveq   #1, r0                      ; ROAD_OP_BASE
    store   r0, (r1)
    addq    #4, r1
    movei   #COMP_BASE, r0
    load    (r0), r0
    store   r0, (r1)
    addq    #4, r1
    movei   #gc_pass, r0
    jump    T, (r0)
    nop

; The list's end.  Then the Blitter must have restored the viewport before a
; single span lands on it: it was started just before this launch and ran
; while the composers worked.  A bounded wait: a stuck Blitter costs a wrong
; frame, not a hung GPU.
gc_end:
    moveq   #0, r0                      ; ROAD_OP_END
    store   r0, (r1)
    addq    #4, r1
    store   r0, (r1)
    movei   #B_CMD, r2
    movei   #$100000, r3
    movei   #gc_bwait, r4
gc_bwait:
    load    (r2), r0
    btst    #0, r0
    jr      NE, gc_bdone                ; bit 0 set: idle
    nop
    subq    #1, r3
    jump    NE, (r4)
    nop
gc_bdone:
    movei   #CMD_BUF, r1
    movei   #gr_start, r0
    jump    T, (r0)
    nop

; fill: r16 = override (0: the record's colour), returns through r17.
; COMP_QUAD0 / COMP_QUAD1 hold each quad's byte for one half: one load.
gc_fill:
    move    r2, r0
    add     r4, r0
    load    (r0), r31                   ; colour << 24 | span block
    addq    #4, r4
    move    r31, r30
    shrq    #24, r30
    cmpq    #0, r16
    jr      EQ, gc_fill_own
    nop
    move    r16, r30
gc_fill_own:
    shlq    #2, r30
    add     r12, r30
    load    (r30), r30                  ; the fill byte
    or      r8, r30
    and     r11, r31
    add     r10, r31
    store   r31, (r1)
    addq    #4, r1
    store   r30, (r1)
    addq    #4, r1
    jump    T, (r17)
    nop

; compose_floor (fn_2e50), returns through r18.
gc_floor:
    move    r5, r24
    and     r13, r24                    ; c = tile & 15
    jump    EQ, (r18)
    nop
    SEEK    0
    move    r24, r16
    movei   #gc_fl1, r17
    jump    T, (r27)
    nop
gc_fl1:
    move    r7, r0
    and     r13, r0
    jr      NE, gc_fl_skip              ; an inner floor: no side edge
    nop
    move    r24, r16
    addq    #30, r16                    ; c + 0x1e
    movei   #gc_fl2, r17
    jump    T, (r27)
    nop
gc_fl_skip:
    SKIP
gc_fl2:
    move    r6, r0
    and     r13, r0
    jump    NE, (r18)                   ; a nearer floor: no front edge
    nop
    move    r24, r16
    addq    #15, r16                    ; c + 0xf
    move    r18, r17                    ; fill returns for floor
    jump    T, (r27)
    nop

; shape 0: compose_floor
gc_c_floor:
    FLOOR
    jump    T, (r29)
    nop

; shape 2: compose_lowblock (0x2e9f)
gc_c_lowblock:
    FLOOR
    movei   #gc_lb1, r30
    SHAPE   r6
    cmpq    #2, r0
    jump    PL, (r30)                   ; shape(nearer) >= 2
    nop
    SEEK    3
    FILL    0
gc_lb1:
    SEEK    2
    BLOCKCOLOR
    movei   #gc_lb2, r17
    jump    T, (r27)
    nop
gc_lb2:
    SHAPE   r7
    cmpq    #2, r0
    jump    PL, (r29)                   ; shape(inner) >= 2
    nop
    FILL    0
    jump    T, (r29)
    nop

; shape 3: compose_tun_low (0x2ee1)
gc_c_tun_low:
    FLOOR
    movei   #gc_tl1, r30
    SHAPE   r6
    cmpq    #2, r0
    jump    PL, (r30)
    nop
    SEEK    1
    FILLI   $41
gc_tl1:
    SEEK    2
    BLOCKCOLOR
    movei   #gc_tl2, r17
    jump    T, (r27)
    nop
gc_tl2:
    movei   #gc_tl3, r30
    SHAPE   r7
    cmpq    #2, r0
    jump    PL, (r30)
    nop
    FILL    0
gc_tl3:
    SHAPE   r6
    cmpq    #2, r0
    jump    PL, (r29)
    nop
    SEEK    3
    SKIP
    FILL    0
    FILL    0
    jump    T, (r29)
    nop

; shape 4: compose_highblock (0x2f3c)
gc_c_highblock:
    FLOOR
    movei   #gc_hb1, r30
    SHAPE   r6
    cmpq    #2, r0
    jump    PL, (r30)
    nop
    SEEK    3
    FILL    0
gc_hb1:
    SEEK    2
    SKIP
    movei   #gc_high_top, r30
    SHAPE   r7
    cmpq    #2, r0
    jump    PL, (r30)
    nop
    FILL    0
    movei   #gc_high_top, r30
    jump    T, (r30)
    nop

; shape 1: compose_tunnel (0x303d)
gc_c_tunnel:
    FLOOR
    movei   #gc_tu1, r30
    SHAPE   r6
    cmpq    #1, r0
    jump    PL, (r30)
    nop
    SEEK    1
    FILLI   $43
gc_tu1:
    SEEK    4
    FILL    0
    FILL    0
    FILL    0
    FILL    0
    FILL    0
    FILL    0
    SHAPE   r6
    cmpq    #1, r0
    jump    PL, (r29)
    nop
    FILL    0
    FILL    0
    jump    T, (r29)
    nop

; shape 5: compose_tun_high (0x2fb0)
gc_c_tun_high:
    FLOOR
    movei   #gc_th1, r30
    SHAPE   r6
    cmpq    #2, r0
    jump    PL, (r30)
    nop
    SEEK    1
    FILLI   $41
gc_th1:
    SEEK    2
    SKIP
    movei   #gc_th2, r30
    SHAPE   r7
    cmpq    #2, r0
    jump    PL, (r30)
    nop
    FILL    0
gc_th2:
    SEEK    4
    FILL    0
    FILL    0
    FILL    0
    FILL    0
    FILL    0
    FILL    0
; compose_highblock and compose_tun_high end alike: the block's top (kind 5)
gc_high_top:
    SEEK    5
    BLOCKCOLOR
    movei   #gc_ht1, r17
    jump    T, (r27)
    nop
gc_ht1:
    movei   #gc_ht2, r30
    SHAPE   r7
    cmpq    #4, r0
    jump    PL, (r30)                   ; shape(inner) >= 4: no edge, skip it
    nop
    FILL    0
    movei   #gc_ht3, r30
    jump    T, (r30)
    nop
gc_ht2:
    SKIP
gc_ht3:
    SHAPE   r6
    cmpq    #4, r0
    jump    PL, (r29)
    nop
    FILL    0
    jump    T, (r29)
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
