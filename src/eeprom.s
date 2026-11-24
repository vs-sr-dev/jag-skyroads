;===========================================================================
; The cartridge EEPROM: 64 words (National NM93C14 family), where
; skyroads.cfg lives (csrc/front.c fe_cfg_load / fe_cfg_save).
;
; Atari's primitives AS THEY ARE (JaguarDocs hw/08 rule 7): EEPRIM.S by Dave
; Staugas, Atari Sunnyvale, 24-Sept-93, the same code Club Drive, Checkered
; Flag and Jaguar Doom ship; only the names are ours.  DO is read on bit 0
; of JOYSTICK, DI written on bit 0 of GPIO_0, CS pulsed by any access to
; GPIO_1.  The pad shares JOYSTICK: here it is read only by the main thread
; (ReadPad), never in an interrupt, so a transaction cannot be cut by a
; strobe (Jaguar Doom waits a VBL for the same reason: its pad is read in
; the interrupt).
;
;   EeReadC    C ABI: uint32_t ee_read(uint32_t word)              via EE_READ_FN
;   EeWriteC   C ABI: int ee_write(uint32_t word, uint32_t data)   via EE_WRITE_FN
;              0 or more = written, -1 = the chip never said ready
;===========================================================================
EE_JOY          EQU     $F14000         ; I/O base (JOYSTICK)
EE_GPIO0        EQU     $F14800-EE_JOY  ; offsets from it
EE_GPIO1        EQU     $F15000-EE_JOY

EE_READ         EQU     $180            ; %110000000  9-bit commands
EE_EWEN         EQU     $130            ; %100110000  erase/write enable
EE_WRITE        EQU     $140            ; %101000000
EE_EWDS         EQU     $100            ; %100000000  erase/write disable

EeReadC:
    move.l  4(sp), d1
    bsr     ee_read
    andi.l  #$FFFF, d0
    rts

EeWriteC:
    move.l  d4, -(sp)
    move.l  8(sp), d1
    move.l  12(sp), d0
    bsr     ee_write
    move.w  d4, d0
    ext.l   d0
    move.l  (sp)+, d4
    rts

; d0.w data, d1.w address (0-63).  Out: d4.w = -1 if no ready came.
ee_write:
    movem.l a0/d0-d3, -(sp)
    lea     EE_JOY, a0
    tst.w   EE_GPIO1(a0)                    ; strobe CS
    move.w  #EE_EWEN, d2
    bsr     ee_out9
    tst.w   EE_GPIO1(a0)
    andi.w  #$3F, d1
    ori.w   #EE_WRITE, d1
    move.w  d1, d2
    bsr     ee_out9
    move.w  d0, d2
    bsr     ee_out16
    tst.w   EE_GPIO1(a0)
    nop                                     ; 1 us after CS for a valid status
    nop
    move.w  #1, d2
    move.w  #32767, d4
.ew_wait:
    move.w  (a0), d3                        ; until the write is complete
    and.w   d2, d3
    bne.s   .ew_done
    dbra    d4, .ew_wait
.ew_done:
    move.w  #EE_EWDS, d2
    bsr     ee_out9
    tst.w   EE_GPIO1(a0)
    movem.l (sp)+, a0/d0-d3
    tst.w   d4
    rts

; d1.w address (0-63).  Out: d0.w data; everything else preserved.
ee_read:
    movem.l a0/d1-d4, -(sp)
    lea     EE_JOY, a0
    tst.w   EE_GPIO1(a0)
    andi.w  #$3F, d1
    ori.w   #EE_READ, d1
    move.w  d1, d2
    bsr     ee_out9
    moveq   #0, d0
    moveq   #15, d3                         ; 16 data bits (after the dummy 0)
.er_bit:
    tst.w   EE_GPIO0(a0)                    ; clock
    nop
    move.w  (a0), d1
    lsr.w   #1, d1
    addx.w  d0, d0
    nop
    nop
    nop
    nop
    nop
    nop
    dbra    d3, .er_bit
    movem.l (sp)+, a0/d1-d4
    rts

; a0 = EE_JOY, d2.w = 16 bits / a 9-bit command, sent MSB first on DI.
; d2, d3 destroyed.
ee_out16:
    rol.w   #1, d2
    moveq   #15, d3
    bra.s   ee_outlp
ee_out9:
    rol.w   #8, d2
    moveq   #8, d3
ee_outlp:
    move.w  d2, EE_GPIO0(a0)
    nop
    nop
    nop
    nop
    nop
    nop
    rol.w   #1, d2
    dbra    d3, ee_outlp
    rts
