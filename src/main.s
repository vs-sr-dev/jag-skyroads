;===========================================================================
; JagSkyroads - M0: boot, 320x240 progressive video, the game screen from the
; cartridge through the Object Processor.
;
; Silicon Relics, 2026.  A port of SkyRoads (Bluemoon Interactive, 1993); the
; game's data come from the user's own copy, converted by tools/sr2jag.py.
;
; What M0 proves, and nothing else: that the boot reaches the main loop on
; real hardware, that the video interrupt fires, and that the converted blobs
; sit where tools/memory_map.py says.  On screen: a world backdrop with the
; dashboard over it, and every 2.5 seconds the next of the ten worlds with its
; road's palette.  The expected frame is build/preview/m0_expected.png.
;
; Every hardware sequence here is COPIED from code verified on a real Jaguar,
; not derived: Prowl's boot.s (quiet, video off before the counters) and Keen
; Dreams' main.s (timing, OP list, interrupt).  Invented init sequences ran on
; BigPEmu and died on hardware in two projects out of two.
;
; ONLY 7-BIT ASCII in this file.  RMAC has NO operator precedence: brackets
; around every mixed expression (tools/check_expr.py checks it in the build).
;
; BG colour = boot stage, readable when nothing else is:
;   dark green   boot did not reach the main loop
;   blue         main loop reached, the video interrupt never fired
;   slow beat    running (two dim colours in the margins, BG_PROBE=1)
;   black        the video is off: the boot died before VideoOnMinimal
;   red          Panic (an exception)
;===========================================================================

    .include "jaguar.inc"
    .include "map.inc"

;---------------------------------------------------------------------------
; Object Processor fields (Keen Dreams, verified against TOM's netlist).
; Phrase 0: TYPE(3) YPOS(11) HEIGHT(10) LINK(19) DATA(21); LINK and DATA are
; addresses / 8 and straddle the two longs.
;---------------------------------------------------------------------------
LINK_HI_STOP    EQU     ((OP_STOP>>11)&$7FF)
LINK_LO_STOP    EQU     ((OP_STOP>>3)&$FF)

OBJ_TYPE_BMP    EQU     0
OBJ_TYPE_BRANCH EQU     3
OBJ_TYPE_STOP   EQU     4

; Branch conditions, from YPOS' point of view: 1 = YPOS > VC, 2 = YPOS < VC.
CC_YPOS_GT      EQU     1
CC_YPOS_LT      EQU     2

; Transparency: bit 47 of phrase 1 = bit 15 of the high long (ob.v:2354).
OBJ_TRANS       EQU     (1<<15)

FB_DEPTH_8BPP   EQU     3
FB_INDEX        EQU     1
FB_RELEASE      EQU     1

GAME_IWIDTH     EQU     (SCR_W/8)

;---------------------------------------------------------------------------
; Video timing: Keen Dreams' values, verified on the user's NTSC Jaguar.
; VP odd = progressive.  VDE = VDB + 2*SCR_H (the counter counts half-lines).
;---------------------------------------------------------------------------
N_HP            EQU     844
N_HBB           EQU     1697
N_HBE           EQU     118
N_HS            EQU     1743
N_HVS           EQU     595
N_HEQ           EQU     780
N_VP            EQU     523
N_VEB           EQU     512
N_VS            EQU     518
N_VEE           EQU     5
N_VBE           EQU     30
N_VBB           EQU     512
N_VDB           EQU     30

P_HP            EQU     850
P_HBB           EQU     1709
P_HBE           EQU     153
P_HS            EQU     1749
P_HVS           EQU     600
P_HEQ           EQU     787
P_VP            EQU     623
P_VEB           EQU     614
P_VS            EQU     619
P_VEE           EQU     4
P_VBE           EQU     40
P_VBB           EQU     614
P_VDB           EQU     40

VD_SPAN         EQU     (SCR_H*2)

; Jaguar RGB16: r5<<11 | b5<<6 | g6 (green at the BOTTOM, blue in the middle).
;   COL_BOOT   $0018  dark green
;   COL_LOOP   $07C0  blue
;   COL_PANIC  $F800  red
;   COL_BLACK  $0000
COL_BOOT        EQU     $0018
COL_LOOP        EQU     $07C0
COL_PANIC       EQU     $F800
COL_BLACK       EQU     $0000
;   COL_BEAT_A $4010  dim red-green, COL_BEAT_B $0410 dim blue-green
COL_BEAT_A      EQU     $4010
COL_BEAT_B      EQU     $0410

; BG_PROBE: while running, BG beats between two dim colours every 32 frames
; in the margins around the game screen, so that BLACK can only mean "video
; off".  M0's first round showed black and could not say whether the boot
; had died or the OP drew nothing.  Off once M0 is closed on hardware.
BG_PROBE        EQU     1

; INT1: every latch cleared and every source off (bits 8-12).
C_ALLCLR        EQU     $1F00
INT1_ENA        EQU     C_VIDENA

; Frames each world stays on screen in the M0 slideshow.
SLIDE_FRAMES    EQU     150

; Boot stages for BOOT_MARK.
BM_START        EQU     1
BM_VIDEO        EQU     2
BM_CLUT         EQU     3
BM_ASSETS       EQU     4
BM_OPLIST       EQU     5
BM_LOOP         EQU     6

;===========================================================================
; THE COLD BOOT RUNS FROM THE CARTRIDGE; EVERYTHING AFTER `jmp Start` FROM DRAM.
;
; The image is assembled at DCODE but sits at ROM_CODE, where the $F6 loader
; jumps.  These instructions are position independent - absolute constants,
; relative dbra, an absolute final jmp - so they work from the wrong address.
;
; 1. quiet: on the GameDrive the previous ROM's GPU, DSP, interrupts and video
;    survive a reset (Prowl, 2026-11-19/20 on hardware);
; 2. video OFF before anything touches the counters: written with the video
;    on, the HC/VC lock came or not by the instant (Prowl, hardware);
; 3. the whole DRAM cleared: on hardware it holds what the BIOS left, and a
;    cell read before it is written breaks only there (Keen Dreams, twice);
; 4. the program copied to DCODE: the 68000 has no instruction cache, and
;    code running from the cartridge holds the bus the OP needs (Keen Dreams,
;    measured: 98% of the frame).
;===========================================================================
    .org    DCODE

ColdBoot:
    move.w  #$2700, sr
    move.l  #0, G_CTRL
    move.l  #0, D_CTRL
    move.w  #0, INT2
    move.w  #C_ALLCLR, INT1
    move.w  #0, VMODE

    moveq   #0, d0
    moveq   #0, d1
    moveq   #0, d2
    moveq   #0, d3
    moveq   #0, d4
    moveq   #0, d5
    moveq   #0, d6
    suba.l  a1, a1
    movea.l #$200000, a0
    move.w  #$FFFF, d7                      ; 65536 x 32 bytes = 2 MB
.cb_clr:
    movem.l d0-d6/a1, -(a0)
    dbra    d7, .cb_clr

    movea.l #ROM_CODE, a0
    movea.l #DCODE, a1
    move.w  #((DCODE_SIZE/32)-1), d7
.cb_copy:
    movem.l (a0)+, d0-d6/a2
    movem.l d0-d6/a2, (a1)
    lea     32(a1), a1
    dbra    d7, .cb_copy

    jmp     Start

;===========================================================================
Start:
    move.w  #$2700, sr
    move.l  #STACK_TOP, a7
    move.l  #BM_START, BOOT_MARK

    bsr     InitVectors
    bsr     InitVideoTiming
    bsr     VideoOnMinimal
    move.l  #BM_VIDEO, BOOT_MARK

    moveq   #1, d0                          ; road 1 = world 0
    bsr     LoadClut
    move.l  #BM_CLUT, BOOT_MARK

    moveq   #0, d0
    bsr     LoadWorld
    bsr     LoadDash
    move.l  #BM_ASSETS, BOOT_MARK

    bsr     InitOpList
    move.l  #BM_OPLIST, BOOT_MARK

; The mask is $2000, NOT $2100, and INT2 is written before INT1: the sequence
; Jaguar Chess invented ran on BigPEmu and died on hardware.
    moveq   #0, d0
    move.l  d0, IRQ_TICK
    move.w  #0, INT2
    move.w  #INT1_ENA|C_VIDCLR, INT1
    move.w  #$2000, sr

    move.l  #BM_LOOP, BOOT_MARK
    move.w  #COL_LOOP, BG

;---------------------------------------------------------------------------
; The M0 loop: wait for the frame, and every SLIDE_FRAMES show the next world
; with the palette of its first road (road = 3*world + 1).
;---------------------------------------------------------------------------
    moveq   #0, d6                          ; frames on this world
    moveq   #0, d7                          ; current world
MainLoop:
    move.l  IRQ_TICK, d5
.ml_sync:
    move.l  IRQ_TICK, d0
    cmp.l   d5, d0
    beq.s   .ml_sync
    .if BG_PROBE=1
    move.w  #COL_BEAT_A, d0
    btst    #5, d5
    beq.s   .ml_beat
    move.w  #COL_BEAT_B, d0
.ml_beat:
    move.w  d0, BG
    .else
    move.w  #COL_BLACK, BG
    .endif

    addq.w  #1, d6
    cmpi.w  #SLIDE_FRAMES, d6
    bne.s   MainLoop
    moveq   #0, d6
    addq.w  #1, d7
    cmpi.w  #N_WORLDS, d7
    bne.s   .ml_w
    moveq   #0, d7
.ml_w:
    move.w  d7, d0
    bsr     LoadWorld
    move.w  d7, d0
    mulu    #3, d0
    addq.w  #1, d0
    bsr     LoadClut
    bra.s   MainLoop

;---------------------------------------------------------------------------
; InitVectors: all 256 to Panic, then $100 (the Jaguar's only interrupt
; vector) to the video handler.
;---------------------------------------------------------------------------
InitVectors:
    movea.l #VECTORS, a0
    move.w  #255, d0
    move.l  #Panic, d1
.iv_loop:
    move.l  d1, (a0)+
    dbra    d0, .iv_loop
    move.l  #ViHandler, $100
    rts

Panic:
    move.w  #$2700, sr
.pa_loop:
    move.w  #COL_PANIC, BG
    bra.s   .pa_loop

;---------------------------------------------------------------------------
; InitVideoTiming: NTSC/PAL detection and 240p timing (Keen Dreams).
; VI must be ODD in progressive (TOM vid.v:3412, vintd = vieq & hdeeq, and
; hc[10] == VC&1 once HC and VC start together from zero).
;---------------------------------------------------------------------------
InitVideoTiming:
    move.w  #0, VMODE
    move.w  #0, HC
    move.w  #0, VC

    move.w  BASE+$14002, d0
    and.w   #$10, d0
    beq     .ivt_pal
    move.w  #1, VID_NTSC
    move.w  #823, d2
    move.w  #1409, d0
    move.w  #N_HP, HP
    move.w  #N_HBB, HBB
    move.w  #N_HBE, HBE
    move.w  #N_HS, HS
    move.w  #N_HVS, HVS
    move.w  #N_HEQ, HEQ
    move.w  #60, TICK_HZ
    move.w  #N_VP, VP
    move.w  #N_VEB, VEB
    move.w  #N_VS, VS
    move.w  #N_VEE, VEE
    move.w  #N_VBE, VBE
    move.w  #N_VBB, VBB
    move.w  #N_VDB, d5
    bra     .ivt_common
.ivt_pal:
    move.w  #0, VID_NTSC
    move.w  #843, d2
    move.w  #1381, d0
    move.w  #P_HP, HP
    move.w  #P_HBB, HBB
    move.w  #P_HBE, HBE
    move.w  #P_HS, HS
    move.w  #P_HVS, HVS
    move.w  #P_HEQ, HEQ
    move.w  #50, TICK_HZ
    move.w  #P_VP, VP
    move.w  #P_VEB, VEB
    move.w  #P_VS, VS
    move.w  #P_VEE, VEE
    move.w  #P_VBE, VBE
    move.w  #P_VBB, VBB
    move.w  #P_VDB, d5
.ivt_common:
    move.w  d0, d1
    asr     #1, d1
    sub.w   d1, d2
    add.w   #4, d2
    sub.w   #1, d1
    or.w    #$400, d1
    move.w  d1, HDE
    move.w  d2, HDB1
    move.w  d2, HDB2

    move.w  d5, VDB
    move.w  d5, VDB_VAL
    move.w  d5, d6
    add.w   #VD_SPAN, d6
    move.w  d6, VDE
    move.w  d6, VDE_VAL

    move.w  d6, d0
    addq.w  #1, d0
    ori.w   #1, d0
    move.w  d0, VI
    move.w  d0, VI_VAL
    move.l  #0, BORD1
    rts

;---------------------------------------------------------------------------
; VideoOnMinimal: a list of one STOP, video on: the screen shows BG only.
; PWIDTH4 = 320 columns; RGB16 because the CLUT entries are RGB16.
;---------------------------------------------------------------------------
VideoOnMinimal:
    move.l  #0, OP_STOP
    move.l  #OBJ_TYPE_STOP, OP_STOP+4
    move.l  #OP_STOP, d0
    swap    d0
    move.l  d0, OLP
    move.w  #COL_BOOT, BG
    move.w  #VIDEN|BGEN|PWIDTH4|RGB16|CSYNC, VMODE
    rts

;---------------------------------------------------------------------------
; LoadClut: d0 = road (0..30).  256 RGB16 entries, composed by sr2jag.py.
;---------------------------------------------------------------------------
LoadClut:
    movem.l d0/a0-a1, -(sp)
    andi.l  #$FF, d0
    lsl.l   #8, d0
    add.l   d0, d0                          ; * CLUT_BYTES (512)
    movea.l #ROM_CLUT, a0
    adda.l  d0, a0
    movea.l #CLUT, a1
    move.w  #127, d0
.lc_loop:
    move.l  (a0)+, (a1)+
    dbra    d0, .lc_loop
    movem.l (sp)+, d0/a0-a1
    rts

;---------------------------------------------------------------------------
; Copy32: a0 -> a1, d0 = bytes / 32 (non zero).  MOVEM, eight longs a round.
;---------------------------------------------------------------------------
Copy32:
    movem.l d0-d7/a2, -(sp)
    move.l  d0, a2
    subq.l  #1, a2
.c32_loop:
    movem.l (a0)+, d0-d7
    movem.l d0-d7, (a1)
    lea     32(a1), a1
    subq.l  #1, a2
    cmpa.l  #-1, a2
    bne.s   .c32_loop
    movem.l (sp)+, d0-d7/a2
    rts

;---------------------------------------------------------------------------
; LoadWorld: d0 = world (0..9) -> BACKDROP_BUF.
;---------------------------------------------------------------------------
LoadWorld:
    movem.l d0/a0-a1, -(sp)
    andi.l  #$FF, d0
    mulu    #WORLD_STRIDE, d0
    movea.l #ROM_WORLDS, a0
    adda.l  d0, a0
    movea.l #BACKDROP_BUF, a1
    move.l  #(WORLD_BYTES/32), d0
    bsr     Copy32
    movem.l (sp)+, d0/a0-a1
    rts

LoadDash:
    movem.l d0/a0-a1, -(sp)
    movea.l #ROM_DASH, a0
    movea.l #DASH_BUF, a1
    move.l  #(DASH_BYTES/32), d0
    bsr     Copy32
    movem.l (sp)+, d0/a0-a1
    rts

;---------------------------------------------------------------------------
; InitOpList:
;   OP_BR_VDE  if YPOS < VC -> STOP  (below the window)
;   OP_BR_VDB  if YPOS > VC -> STOP  (above)
;   the objects, in OBJ_ order, each linking to the next; the last to STOP.
; The truth lives in OBJ_SHADOW; ArmObjects copies it into the list, and the
; video handler does it again every frame because the OP consumes HEIGHT
; and DATA as it draws.
;---------------------------------------------------------------------------
InitOpList:
    movea.l #OP_LIST, a0
    move.w  #((OP_LIST_SIZE/8)-1), d0
.iol_stop:
    move.l  #0, (a0)+
    move.l  #OBJ_TYPE_STOP, (a0)+
    dbra    d0, .iol_stop

    move.l  #LINK_HI_STOP, OP_BR_VDE
    moveq   #0, d0
    move.w  VDE_VAL, d0
    lsl.l   #3, d0
    or.l    #((LINK_LO_STOP<<24)|(CC_YPOS_LT<<14)|OBJ_TYPE_BRANCH), d0
    move.l  d0, OP_BR_VDE+4

    move.l  #LINK_HI_STOP, OP_BR_VDB
    moveq   #0, d0
    move.w  VDB_VAL, d0
    lsl.l   #3, d0
    or.l    #((LINK_LO_STOP<<24)|(CC_YPOS_GT<<14)|OBJ_TYPE_BRANCH), d0
    move.l  d0, OP_BR_VDB+4

; The world backdrop: opaque, the game screen's rows 0..137.
    lea     OBJ_SHADOW+(OBJ_BACKDROP*OBJ_STRIDE), a0
    move.l  #BACKDROP_BUF, d0
    moveq   #0, d1
    move.w  VDB_VAL, d1
    add.w   #(GAME_Y*2), d1
    move.l  #VIEW_H, d2
    moveq   #0, d3
    move.l  #GAME_IWIDTH, d4
    move.l  #GAME_IWIDTH, d5
    moveq   #0, d7
    bsr     MakeObj

; The dashboard: transparent, rows 129..199.  Its index-0 pixels show what is
; under it: the viewport in rows 129..137, the BG (black) below.
    lea     OBJ_SHADOW+(OBJ_DASH*OBJ_STRIDE), a0
    move.l  #DASH_BUF, d0
    moveq   #0, d1
    move.w  VDB_VAL, d1
    add.w   #((GAME_Y+DASH_Y)*2), d1
    move.l  #DASH_H, d2
    moveq   #0, d3
    move.l  #GAME_IWIDTH, d4
    move.l  #GAME_IWIDTH, d5
    move.l  #OBJ_TRANS, d7
    bsr     MakeObj

    bsr     ArmObjects
    move.l  #OP_BR_VDE, d0
    swap    d0
    move.l  d0, OLP
    rts

;---------------------------------------------------------------------------
; ArmObjects: OBJ_SHADOW -> OP_OBJS, 32 bytes per object, no arithmetic.
;---------------------------------------------------------------------------
ArmObjects:
    movem.l d0-d7/a0-a2, -(sp)
    lea     OBJ_SHADOW, a0
    lea     OP_OBJS, a1
    move.w  #(OBJ_N-1), d0
.ao_loop:
    movem.l (a0)+, d1-d7/a2
    movem.l d1-d7/a2, (a1)
    lea     OBJ_STRIDE(a1), a1
    dbra    d0, .ao_loop
    movem.l (sp)+, d0-d7/a0-a2
    rts

;---------------------------------------------------------------------------
; MakeObj (Keen Dreams): a bitmap object into the shadow cell at a0.
;   d0 data   d1 YPOS (half-lines)   d2 HEIGHT   d3 XPOS
;   d4 IWIDTH (phrases)   d5 DWIDTH (phrases)   d7 extra high-phrase-1 bits
; The LINK is the next cell, always: the last object's next cell is OP_STOP.
;---------------------------------------------------------------------------
MakeObj:
    movem.l d0-d7, -(sp)
    move.l  d7, 8(a0)
    move.l  a0, d7
    sub.l   #OBJ_SHADOW, d7
    add.l   #(OP_OBJS+OBJ_STRIDE), d7
    lsr.l   #3, d7

    lsr.l   #3, d0
    lsl.l   #8, d0
    lsl.l   #3, d0
    move.l  d7, d6
    lsr.l   #8, d6
    andi.l  #$000007FF, d6
    or.l    d6, d0
    move.l  d0, (a0)

    move.l  d7, d0
    andi.l  #$000000FF, d0
    swap    d0
    lsl.l   #8, d0
    move.l  d2, d6
    lsl.l   #8, d6
    lsl.l   #6, d6
    or.l    d6, d0
    move.l  d1, d6
    lsl.l   #3, d6
    or.l    d6, d0
    or.l    #OBJ_TYPE_BMP, d0
    move.l  d0, 4(a0)

    move.l  d4, d0
    lsr.l   #4, d0
    or.l    #((FB_INDEX<<6)|(FB_RELEASE<<16)), d0
    or.l    8(a0), d0
    move.l  d0, 8(a0)

    move.l  d3, d0
    andi.l  #$00000FFF, d0
    or.l    #((FB_DEPTH_8BPP<<12)|(1<<15)), d0
    move.l  d5, d6
    lsl.l   #8, d6
    lsl.l   #8, d6
    lsl.l   #2, d6
    or.l    d6, d0
    move.l  d4, d6
    andi.l  #$0000000F, d6
    swap    d6
    lsl.l   #8, d6
    lsl.l   #4, d6
    or.l    d6, d0
    move.l  d0, 12(a0)
    movem.l (sp)+, d0-d7
    rts

;---------------------------------------------------------------------------
; ViHandler: count, re-arm the list, acknowledge (C_VIDCLR + INT2).
;---------------------------------------------------------------------------
ViHandler:
    addq.l  #1, IRQ_TICK
    bsr     ArmObjects
    move.w  #INT1_ENA|C_VIDCLR, INT1
    move.w  #0, INT2
    rte

CodeEnd:
    .end
