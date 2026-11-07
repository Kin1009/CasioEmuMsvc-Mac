; Build tags: a line ending in ";@2p" is kept only for 2-bitplane targets (cwii), ";@1p" only for
; 1-bitplane targets (cw). The plane loops step R12 = 0,4 (and select the plane through 0F037H) on cwii;
; on cw the same loop runs once and never touches 0F037H.
; ---- Wiz runtime. Convention: args ER0,ER2,ER4 ; result ER0 (ER2 = remainder for div).
; Routines may clobber ER2-ER14. Leaf routines return with RT; the few that BL to another
; routine (__buffered, __key_hit) do PUSH LR ... POP PC like compiled functions.
;
; Shadow model: every drawing primitive writes into the shadow buffer (SB2).
; Only render() copies SB2 into the visible LCD buffer (SB1) at 0xF800.
;   SB1 = 0xF800-0xFFFF
;   SB2 = 0xDDD3-0xE5D2 (cw)  /  0xCA54-0xE253 (cwii)
; _SB2 is defined by the build pipeline for the selected target.
;   0xE808-0xE80F  key_hit edge-detect bitmap (1 bit per scan index)

; ER0 * ER2 -> ER0 (low 16 bits)
__mul16:
    MOV ER4, ER0
    MOV ER0, #0
    MOV R6, #16
__mul16_l:
    TB R2.0
    BEQ __mul16_s
    ADD ER0, ER4
__mul16_s:
    ADD ER4, ER4
    SRLC R2, #1
    SRL R3, #1
    ADD R6, #-1
    BNE __mul16_l
    RT

; signed ER0 / ER2 -> ER0 quotient (trunc to zero), ER2 remainder (sign of dividend)
__div16:
    MOV R6, R2
    OR R6, R3
    BNE __div16_ok
    MOV ER0, #0
    MOV ER2, #0
    RT
__div16_ok:
    MOV R7, #0
    TB R1.7
    BEQ __div16_a
    MOV ER4, #0
    SUB R4, R0
    SUBC R5, R1
    MOV ER0, ER4
    MOV R7, #3
__div16_a:
    TB R3.7
    BEQ __div16_b
    MOV ER4, #0
    SUB R4, R2
    SUBC R5, R3
    MOV ER2, ER4
    XOR R7, #1
__div16_b:
    MOV ER4, #0
    MOV R6, #16
__div16_l:
    ADD ER0, ER0
    ADDC R4, R4
    ADDC R5, R5
    CMP R4, R2
    CMPC R5, R3
    BLT __div16_n
    SUB R4, R2
    SUBC R5, R3
    OR R0, #1
__div16_n:
    ADD R6, #-1
    BNE __div16_l
    MOV ER2, ER4
    TB R7.0
    BEQ __div16_q
    MOV ER4, #0
    SUB R4, R0
    SUBC R5, R1
    MOV ER0, ER4
__div16_q:
    TB R7.1
    BEQ __div16_r
    MOV ER4, #0
    SUB R4, R2
    SUBC R5, R3
    MOV ER2, ER4
__div16_r:
    RT

; ER0 << R2
__shl16:
    CMP R2, #0
    BEQ __shl16_e
__shl16_l:
    ADD ER0, ER0
    ADD R2, #-1
    BNE __shl16_l
__shl16_e:
    RT

; ER0 >> R2 (arithmetic)
__sra16:
    CMP R2, #0
    BEQ __sra16_e
__sra16_l:
    SRLC R0, #1
    SRA R1, #1
    ADD R2, #-1
    BNE __sra16_l
__sra16_e:
    RT

; pixel(x=ER0, y=ER2, c=R4) : c=0 clears, nonzero sets. Off-screen coordinates are ignored.
; Draws into the shadow buffer SB2; call render() to make it visible.
__pixel:
    CMP R1, #0
    BNE __pixel_x
    CMP R0, #0C0H
    BGE __pixel_x
    CMP R3, #0
    BNE __pixel_x
    CMP R2, #_ROWS
    BGE __pixel_x
    MOV R9, R4
    MOV R10, #32
    MOV R6, R2
    MUL ER6, R10
    MOV R10, R0
    SRL R10, #3
    MOV R11, #0
    ADD ER6, ER10
    ADD ER6, #32 ;@off
    MOV R10, #(_SB2 & 255)
    ADD R6, R10
    ADDC R7, #((_SB2 >> 8) & 255)
    MOV R8, #80H
    MOV R10, R0
    AND R10, #7
    SRL R8, R10
    MOV R11, R8
    XOR R11, #0FFH
    L R10, [ER6]
    CMP R9, #0
    BEQ __pixel_c
    OR R10, R8
    BAL __pixel_w
__pixel_c:
    AND R10, R11
__pixel_w:
    ST R10, [ER6]
__pixel_x:
    RT

; clear(): zero the whole shadow buffer (SB2).
__clear:
    MOV R6, #(_SB2 & 255)
    MOV R7, #((_SB2 >> 8) & 255)
    MOV ER10, #0
    MOV R8, #8
__clear_o:
    MOV R9, #0
__clear_i:
    ST R10, [ER6]
    ADD ER6, #1
    ADD R9, #-1
    BNE __clear_i
    ADD R8, #-1
    BNE __clear_o
    RT

; buffered(on): retained for source compatibility; drawing is always shadowed now.
__buffered:
    RT

; clearRender(): clear SB1 first, then SB2 (immediately).
__clearRender:
    MOV R6, #(_SB1 & 255)
    MOV R7, #((_SB1 >> 8) & 255)
    MOV ER10, #0
    MOV R8, #8
__clearRender_o:
    MOV R9, #0
__clearRender_i:
    ST ER10, [ER6]
    ADD ER6, #1
    ADD R9, #-1
    BNE __clearRender_i
    ADD R8, #-1
    BNE __clearRender_o
    B __clear

; flip(): legacy alias for render().
__flip:
    B __render

; render(): copy the shadow buffer (SB2) to the visible LCD plane(s) (SB1).
; On cwii both bitplanes are refreshed from the same monochrome shadow.
__render:
    MOV R12, #0
__render_p:
    ST R12, 0F037H ;@2p
    MOV R6, #(_SB2 & 255)
    MOV R7, #((_SB2 >> 8) & 255)
    MOV R10, #0
    MOV R11, #0F8H
    MOV R8, #8
__render_o:
    MOV R9, #0
__render_i:
    L R4, [ER6]
    ST R4, [ER10]
    ADD ER6, #1
    ADD ER10, #1
    ADD R9, #-1
    BNE __render_i
    ADD R8, #-1
    BNE __render_o
    ADD R12, #4
    CMP R12, #8 ;@2p
    CMP R12, #4 ;@1p
    BNE __render_p
    RT

; renderStatus(): copy only the 23-byte status-icon region from SB2 to LCD SB1.
; On cwii both LCD planes are refreshed; the icon map's final slot is at offset 22.
__renderStatus:
    MOV R12, #0
__renderStatus_p:
    ST R12, 0F037H ;@2p
    MOV R6, #(_SB2 & 255)
    MOV R7, #((_SB2 >> 8) & 255)
    MOV R10, #0
    MOV R11, #0F8H
    MOV R8, #23
__renderStatus_l:
    L R4, [ER6]
    ST R4, [ER10]
    ADD ER6, #1
    ADD ER10, #1
    ADD R8, #-1
    BNE __renderStatus_l
    ADD R12, #4
    CMP R12, #8 ;@2p
    CMP R12, #4 ;@1p
    BNE __renderStatus_p
    RT

; runtime state init (called once at entry, before top-level code)
__rt_init:
    MOV R0, #0
    ST R0, 0E800H
    ST R0, 0E808H
    ST R0, 0E809H
    ST R0, 0E80AH
    ST R0, 0E80BH
    ST R0, 0E80CH
    ST R0, 0E80DH
    ST R0, 0E80EH
    ST R0, 0E80FH
    ST R0, 0E805H           ; ticks(): first-call arming flag
    ; dynamic heap: one free block E820..EF00, file list head 0
    MOV R0, #20H
    ST R0, 0E818H
    MOV R0, #0E8H
    ST R0, 0E819H
    MOV R0, #0
    ST R0, 0E81AH
    MOV R0, #0EFH
    ST R0, 0E81BH
    ST R0, 0E81CH
    ST R0, 0E81DH
    MOV R0, #0E0H
    ST R0, 0E820H
    MOV R0, #86H
    ST R0, 0E821H
    RT

; cwapp only: check MENU (scan index 24) without a call, so LR stays intact.
; The check is inlined at each key entry point below.
; key_down(k=ER0) -> ER0 = 1 while scan index k is held, else 0
__key_down:
    PUSH ER0                   ;@cwapp
    PUSH ER2                   ;@cwapp
    PUSH ER4                   ;@cwapp
    MOV R2, #10H               ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #80H               ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_menu_ok1     ;@cwapp
    B __exit                   ;@cwapp
__cwapp_menu_ok1:            ;@cwapp
    MOV R2, #1                 ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #80H               ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_off_ok1      ;@cwapp
    MOV R2, #10H               ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #4                 ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_off_ok1      ;@cwapp
    B __cwapp_off              ;@cwapp
__cwapp_off_ok1:             ;@cwapp
    POP ER4                    ;@cwapp
    POP ER2                    ;@cwapp
    POP ER0                    ;@cwapp
__key_down_raw:
    AND R0, #3FH
    MOV R2, R0
    SRL R2, #3
    MOV R3, #80H
    SRL R3, R2
    AND R0, #7
    MOV R4, #80H
    SRL R4, R0
    ST R3, 0F046H
    L R5, 0F040H
    AND R5, R4
    BNE __key_down_n
    MOV ER0, #1
    RT
__key_down_n:
    MOV ER0, #0
    RT

; key_hit(k=ER0) -> ER0 = 1 only on the call where k went from released to held
; (call it every loop iteration for every key you care about)
__key_hit:
    PUSH ER0                   ;@cwapp
    PUSH ER2                   ;@cwapp
    PUSH ER4                   ;@cwapp
    MOV R2, #10H               ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #80H               ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_menu_ok2     ;@cwapp
    B __exit                   ;@cwapp
__cwapp_menu_ok2:            ;@cwapp
    MOV R2, #1                 ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #80H               ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_off_ok2      ;@cwapp
    MOV R2, #10H               ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #4                 ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_off_ok2      ;@cwapp
    B __cwapp_off              ;@cwapp
__cwapp_off_ok2:             ;@cwapp
    POP ER4                    ;@cwapp
    POP ER2                    ;@cwapp
    POP ER0                    ;@cwapp
    PUSH LR
    PUSH ER0
    BL __key_down_raw
    POP ER2
    AND R2, #3FH
    MOV R4, R2
    SRL R4, #3
    MOV R5, #0
    MOV R6, #8
    MOV R7, #0E8H
    ADD ER6, ER4
    AND R2, #7
    MOV R8, #80H
    SRL R8, R2
    L R9, [ER6]
    CMP R0, #0
    BEQ __key_hit_up
    MOV R10, R9
    AND R10, R8
    BNE __key_hit_no
    OR R9, R8
    ST R9, [ER6]
    MOV ER0, #1
    POP PC
__key_hit_up:
    MOV R10, R8
    XOR R10, #0FFH
    AND R9, R10
    ST R9, [ER6]
__key_hit_no:
    MOV ER0, #0
    POP PC

; key() -> ER0 = scan index 0..63 (8*KO bit + KI bit, MSB first) or -1
__key:
    PUSH ER0                   ;@cwapp
    PUSH ER2                   ;@cwapp
    PUSH ER4                   ;@cwapp
    MOV R2, #10H               ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #80H               ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_menu_ok3     ;@cwapp
    B __exit                   ;@cwapp
__cwapp_menu_ok3:            ;@cwapp
    MOV R2, #1                 ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #80H               ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_off_ok3      ;@cwapp
    MOV R2, #10H               ;@cwapp
    ST R2, 0F046H              ;@cwapp
    MOV R3, #4                 ;@cwapp
    L R4, 0F040H               ;@cwapp
    AND R4, R3                 ;@cwapp
    BNE __cwapp_off_ok3      ;@cwapp
    B __cwapp_off              ;@cwapp
__cwapp_off_ok3:             ;@cwapp
    POP ER4                    ;@cwapp
    POP ER2                    ;@cwapp
    POP ER0                    ;@cwapp
    MOV R0, #0
    MOV R1, #0
    MOV R2, #80H
__key_x:
    ST R2, 0F046H
    MOV R3, #80H
__key_y:
    L R4, 0F040H
    AND R4, R3
    BEQ __key_f
    ADD R0, #1
    SRL R3, #1
    CMP R3, #0
    BNE __key_y
    SRL R2, #1
    CMP R2, #0
    BNE __key_x
    MOV ER0, #-1
__key_f:
    RT

; cwapp only: Shift+AC (OFF) mirrors the stock power-off path: turn the LCD
; off, clear FCON, mask every interrupt, program STOPACP, and enter STOP.
; ON then repowers the calculator and the firmware relaunches the app.
__cwapp_off:                   ;@cwapp
    MOV R0, #0                 ;@cwapp
    ST R0, 0F03DH              ;@cwapp
    ST R0, 0F00AH              ;@cwapp
    MOV R2, #10H               ;@cwapp
    MOV R3, #0F0H              ;@cwapp
    MOV R4, #8                 ;@cwapp
__cwapp_off_m:                 ;@cwapp
    ST R0, [ER2]               ;@cwapp
    ADD ER2, #1                ;@cwapp
    ADD R4, #-1                ;@cwapp
    BNE __cwapp_off_m          ;@cwapp
    MOV R0, #50H               ;@cwapp
    ST R0, 0F008H              ;@cwapp
    MOV R0, #0A0H              ;@cwapp
    ST R0, 0F008H              ;@cwapp
    MOV R0, #2                 ;@cwapp
    ST R0, 0F009H              ;@cwapp
    NOP                        ;@cwapp
    NOP                        ;@cwapp
__cwapp_off_w:                 ;@cwapp
    B __cwapp_off_w            ;@cwapp

; delay(n): n * ~ (busy loop) -- UNCALIBRATED, units arbitrary until measured on hardware
__delay:
    MOV ER4, ER0
__delay_o:
    MOV R6, #0
    MOV R7, #1
__delay_i:
    ADD ER6, #-1
    BNE __delay_i
    ADD ER4, #-1
    BNE __delay_o
    RT

; peek(addr=ER0) -> ER0 = unsigned byte at addr (RAM, SFR or ROM)
__peek:
    MOV ER2, ER0
    L R0, [ER2]
    MOV R1, #0
    RT

; _rpeek(addr=ER0) -> ER0: like peek, but in data segment _ROMSEG (ROM tables/strings of a program that lives in segment 3)
__rpeek:  ;@seg
    MOV ER2, ER0  ;@seg
    DSRI _ROMSEG  ;@seg
    L R0, [ER2]  ;@seg
    MOV R1, #0  ;@seg
    RT  ;@seg

; poke(addr=ER0, v=ER2): store the low byte of v at addr
__poke:
    ST R2, [ER0]
    RT

; _hspan(x0=ER0, x1=ER2, y=ER4, c=R6): fill pixels x0..x1-1 on row y. Caller guarantees 0<=x0<x1<=192, 0<=y<_ROWS.
; c=0 clears, else sets. Draws into the shadow buffer SB2; call render() to make it visible.
__hspan:
    MOV R8, R4
    MOV R9, #0
    MOV R10, #32
    MUL ER8, R10
    MOV R11, R0
    SRL R11, #3
    MOV R10, R0
    AND R10, #7
    MOV R7, #0FFH
    SRL R7, R10
    MOV R12, R2
    ADD R12, #-1
    MOV R13, R12
    SRL R13, #3
    AND R12, #7
    MOV R3, #7
    SUB R3, R12
    MOV R5, #0FFH
    SLL R5, R3
    SUB R13, R11
    MOV R10, R11
    MOV R11, #0
    ADD ER8, ER10
    ADD ER8, #32 ;@off
    MOV R10, #(_SB2 & 255)
    ADD R8, R10
    ADDC R9, #((_SB2 >> 8) & 255)
    MOV ER10, ER8
    CMP R13, #0
    BNE __hs_multi
    MOV R3, R7
    AND R3, R5
    L R4, [ER10]
    CMP R6, #0
    BEQ __hs_s0
    OR R4, R3
    BAL __hs_s1
__hs_s0:
    XOR R3, #0FFH
    AND R4, R3
__hs_s1:
    ST R4, [ER10]
    BAL __hs_next
__hs_multi:
    L R4, [ER10]
    MOV R3, R7
    CMP R6, #0
    BEQ __hs_f0
    OR R4, R3
    BAL __hs_f1
__hs_f0:
    XOR R3, #0FFH
    AND R4, R3
__hs_f1:
    ST R4, [ER10]
    ADD ER10, #1
    MOV R3, R13
    ADD R3, #-1
    BEQ __hs_last
__hs_mid:
    MOV R4, #0
    CMP R6, #0
    BEQ __hs_m0
    MOV R4, #0FFH
__hs_m0:
    ST R4, [ER10]
    ADD ER10, #1
    ADD R3, #-1
    BNE __hs_mid
__hs_last:
    L R4, [ER10]
    MOV R3, R5
    CMP R6, #0
    BEQ __hs_l0
    OR R4, R3
    BAL __hs_l1
__hs_l0:
    XOR R3, #0FFH
    AND R4, R3
__hs_l1:
    ST R4, [ER10]
__hs_next:
    RT

; wipe(): clear every byte of RAM 0xD000..0xFFFF, then halt.
; Uses registers only, so it can safely erase its own stack/return frame.
__wipe:
    MOV R0, #0
    MOV R1, #0D0H
__wipe_l:
    MOV R2, #0
    ST R2, [ER0]
    ADD ER0, #1
    BNE __wipe_l
__wipe_h:
    B __wipe_h

; reset(): jump to the stock reset stub at 0:A1CE.
_RESET_ADDR EQU 0A1CEH
__reset:
    B _RESET_ADDR

; recover(): wipe RAM except the DBD0..DBDF integrity span, then reset.
; Clears D16C..DBCF, DBE0..FFFF, then D000..D16B last. Register-only.
__recover:
    MOV R0, #6CH
    MOV R1, #0D1H
    MOV R2, #64H
    MOV R3, #0AH
__rec1:
    MOV R4, #0
    ST R4, [ER0]
    ADD ER0, #1
    ADD ER2, #-1
    BNE __rec1
    MOV R0, #0E0H
    MOV R1, #0DBH
    MOV R2, #20H
    MOV R3, #24H
__rec2:
    MOV R4, #0
    ST R4, [ER0]
    ADD ER0, #1
    ADD ER2, #-1
    BNE __rec2
    MOV R0, #0
    MOV R1, #0D0H
    MOV R2, #6CH
    MOV R3, #01H
__rec3:
    MOV R4, #0
    ST R4, [ER0]
    ADD ER0, #1
    ADD ER2, #-1
    BNE __rec3
    B __reset

; ---- Timer0: ticks() / delay_ms() (ported from upstream) ------------------
; TM0D = 0F020H, TM0C = 0F022H, TM0CON = 0F024H/0F025H; 8 ticks per ms.
__tm0_start:
    PUSH LR
    L R0, 0F00AH
    AND R0, #0FDH
    ST R0, 0F00AH
    MOV R0, #0FFH
    ST R0, 0F020H
    ST R0, 0F021H
    MOV R0, #0
    ST R0, 0F022H
    ST R0, 0F023H
    MOV R0, #1
    ST R0, 0F024H
    ST R0, 0F025H
    POP PC

__tm0_read:
    PUSH LR
__tm0_read_l:
    L R1, 0F023H
    L R0, 0F022H
    L R2, 0F023H
    CMP R1, R2
    BNE __tm0_read_l
    POP PC

__ticks:
    PUSH LR
    L R0, 0E805H
    BNE __ticks_chk
    MOV R0, #1
    ST R0, 0E805H
    BL __tm0_start
    B __ticks_r
__ticks_chk:
    BL __tm0_read
    CMP R0, #0FFH
    BNE __ticks_r
    CMP R1, #0FFH
    BNE __ticks_r
    BL __tm0_start
__ticks_r:
    BL __tm0_read
    POP PC

__delay_ms:
    PUSH LR
    CMP R1, #0
    BLTS __delay_ms_out
    MOV ER8, ER0
    MOV R6, R0
    MOV R7, R1
    BL __tm0_read
    PUSH ER0
__delay_ms_full:
    CMP R7, #0
    BEQ __delay_ms_part
    MOV R2, #0
    MOV R3, #1
    BL __delay_chunk
    ADD R7, #0FFH
    B __delay_ms_full
__delay_ms_part:
    CMP R6, #0
    BEQ __delay_ms_fix
    MOV R2, R6
    MOV R3, #0
    BL __delay_chunk
__delay_ms_fix:
    POP ER2
    MOV ER4, ER8
    ADD ER4, ER4
    ADD ER4, ER4
    ADD ER4, ER4
    ADD ER2, ER4
    ST R2, 0F022H
    ST R3, 0F023H
__delay_ms_out:
    POP PC

__delay_chunk:
    PUSH LR
    BL __tm0_start
    MOV ER4, ER2
    ADD ER4, ER4
    ADD ER4, ER4
    ADD ER4, ER4
__delay_chunk_w:
    BL __tm0_read
    CMP R0, R4
    CMPC R1, R5
    BLTS __delay_chunk_w
    POP PC

; ---- heap byte access ----------------------------------------------------
__hpeek:
    MOV ER2, ER0
    L R0, [ER2]
    MOV R1, #0
    RT

__hpoke:
    ST R2, [ER0]
    RT

; ---- C23/C89 variadic helpers -------------------------------------------
; The prologue of a variadic function stores the first-extra-argument array
; at 0E806H; va_start returns it and va_arg advances one 16-bit slot.
__va_start:
    L ER0, 0E806H
    RT

__va_arg:
    MOV ER4, ER0        ; ER4 = &ap
    L ER2, [ER4]        ; ER2 = ap (pointer into the argument array)
    L ER0, [ER2]        ; value
    ADD ER2, #2
    ST ER2, [ER4]       ; *(&ap) = ap + 2
    RT

; cwapp only: #preserve / preserve() / restore_preserved().
; The compiler allocates __cwapp_preserve_state (see build.hpp):
;   +0 count, +1 used snapshot bytes, +2 8 x (addr lo, addr hi, len lo, len hi),
;   +34 128 snapshot bytes.  __preserve takes ER0=addr, ER2=len and returns
;   ER0=1 on success, 0 on a rejected range or a full table.
__preserve:                    ;@cwapp ;@preserve
    PUSH ER2                   ;@cwapp ;@preserve
    PUSH ER4                   ;@cwapp ;@preserve
    PUSH ER6                   ;@cwapp ;@preserve
    PUSH ER8                   ;@cwapp ;@preserve
    PUSH ER10                  ;@cwapp ;@preserve
    PUSH ER12                  ;@cwapp ;@preserve
    PUSH ER14                  ;@cwapp ;@preserve
    MOV ER6, ER0               ;@cwapp ;@preserve
    MOV ER8, ER2               ;@cwapp ;@preserve
    MOV ER0, #0                ;@cwapp ;@preserve
    CMP ER8, ER0               ;@cwapp ;@preserve
    BEQ __pres_fail            ;@cwapp ;@preserve
    MOV R0, R9                 ;@cwapp ;@preserve
    CMP R0, #0                 ;@cwapp ;@preserve
    BNE __pres_fail            ;@cwapp ;@preserve
    ; addr must be 0xD000..0xFFFF (high byte 0xD0..0xFF) ;@preserve
    TB R7.7                    ;@cwapp ;@preserve
    BEQ __pres_fail            ;@cwapp ;@preserve
    MOV R0, R7                 ;@cwapp ;@preserve
    CMP R0, #0D0H              ;@cwapp ;@preserve
    BLTS __pres_fail           ;@cwapp ;@preserve
    ; end = addr + len; reject a 16-bit wrap ;@preserve
    MOV ER12, ER6              ;@cwapp ;@preserve
    ADD ER12, ER8              ;@cwapp ;@preserve
    CMP ER12, ER6              ;@cwapp ;@preserve
    BLTS __pres_fail           ;@cwapp ;@preserve
    ; reject 0xE5D3..0xE7FF ;@preserve
    MOV R0, #0FFH              ;@cwapp ;@preserve
    MOV R1, #0E7H              ;@cwapp ;@preserve
    CMP ER6, ER0               ;@cwapp ;@preserve
    BGTS __pres_area_ok        ;@cwapp ;@preserve
    MOV R0, #0D3H              ;@cwapp ;@preserve
    MOV R1, #0E5H              ;@cwapp ;@preserve
    MOV ER10, ER12             ;@cwapp ;@preserve
    ADD ER10, #-1              ;@cwapp ;@preserve
    CMP ER10, ER0               ;@cwapp ;@preserve
    BLTS __pres_area_ok        ;@cwapp ;@preserve
    B __pres_fail              ;@cwapp ;@preserve
__pres_area_ok:                ;@cwapp ;@preserve
    MOV R0, #(__cwapp_preserve_state & 0FFH)       ;@cwapp ;@preserve
    MOV R1, #((__cwapp_preserve_state >> 8) & 0FFH);@cwapp ;@preserve
    L R14, [ER0]               ;@cwapp ;@preserve
    CMP R14, #8                ;@cwapp ;@preserve
    BGE __pres_fail            ;@cwapp ;@preserve
    MOV ER2, ER0               ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    L R15, [ER2]               ;@cwapp ;@preserve
    ; used + len must be 1..128 ;@preserve
    MOV R0, R15                ;@cwapp ;@preserve
    MOV R1, #0                 ;@cwapp ;@preserve
    MOV ER4, ER8               ;@cwapp ;@preserve
    ADD ER4, ER0               ;@cwapp ;@preserve
    MOV R0, #128               ;@cwapp ;@preserve
    MOV R1, #0                 ;@cwapp ;@preserve
    CMP ER4, ER0               ;@cwapp ;@preserve
    BGTS __pres_fail           ;@cwapp ;@preserve
    MOV R0, #0                 ;@cwapp ;@preserve
    CMP ER4, ER0               ;@cwapp ;@preserve
    BEQ __pres_fail            ;@cwapp ;@preserve
    ; descriptor pointer = state + 2 + count*4 ;@preserve
    MOV R0, #(__cwapp_preserve_state & 0FFH)       ;@cwapp ;@preserve
    MOV R1, #((__cwapp_preserve_state >> 8) & 0FFH);@cwapp ;@preserve
    MOV ER2, ER0               ;@cwapp ;@preserve
    ADD ER2, #2                ;@cwapp ;@preserve
    MOV R0, R14                ;@cwapp ;@preserve
    MOV R1, #0                 ;@cwapp ;@preserve
    ADD ER2, ER0               ;@cwapp ;@preserve
    ADD ER2, ER0               ;@cwapp ;@preserve
    ADD ER2, ER0               ;@cwapp ;@preserve
    ADD ER2, ER0               ;@cwapp ;@preserve
    ST R6, [ER2]               ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    ST R7, [ER2]               ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    ST R8, [ER2]               ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    ST R9, [ER2]               ;@cwapp ;@preserve
    ; snapshot pointer = state + 34 + used ;@preserve
    MOV R0, #(__cwapp_preserve_state & 0FFH)      ;@cwapp ;@preserve
    MOV R1, #((__cwapp_preserve_state >> 8) & 0FFH);@cwapp ;@preserve
    MOV ER10, ER0              ;@cwapp ;@preserve
    ADD ER10, #34              ;@cwapp ;@preserve
    MOV R0, R15                ;@cwapp ;@preserve
    MOV R1, #0                 ;@cwapp ;@preserve
    ADD ER10, ER0              ;@cwapp ;@preserve
__pres_copy:                   ;@cwapp ;@preserve
    L R0, [ER6]                ;@cwapp ;@preserve
    ST R0, [ER10]              ;@cwapp ;@preserve
    ADD ER6, #1                ;@cwapp ;@preserve
    ADD ER10, #1               ;@cwapp ;@preserve
    CMP ER6, ER12              ;@cwapp ;@preserve
    BNE __pres_copy            ;@cwapp ;@preserve
    ; count++, used += len ;@preserve
    MOV R0, #(__cwapp_preserve_state & 0FFH)       ;@cwapp ;@preserve
    MOV R1, #((__cwapp_preserve_state >> 8) & 0FFH);@cwapp ;@preserve
    ADD R14, #1                ;@cwapp ;@preserve
    ST R14, [ER0]              ;@cwapp ;@preserve
    ADD ER0, #1                ;@cwapp ;@preserve
    ADD R15, R8                ;@cwapp ;@preserve
    ST R15, [ER0]              ;@cwapp ;@preserve
    MOV ER0, #1                ;@cwapp ;@preserve
    B __pres_done              ;@cwapp ;@preserve
__pres_fail:                   ;@cwapp ;@preserve
    MOV ER0, #0                ;@cwapp ;@preserve
__pres_done:                   ;@cwapp ;@preserve
    POP ER14                   ;@cwapp ;@preserve
    POP ER12                   ;@cwapp ;@preserve
    POP ER10                   ;@cwapp ;@preserve
    POP ER8                    ;@cwapp ;@preserve
    POP ER6                    ;@cwapp ;@preserve
    POP ER4                    ;@cwapp ;@preserve
    POP ER2                    ;@cwapp ;@preserve
    RT                         ;@cwapp ;@preserve

; Restore every registered range, byte for byte.  Called by the clean return
; before the firmware state is handed back, and available to programs that
; mutate a preserved range in the middle of a run.
__restore_preserved:           ;@cwapp ;@preserve
    PUSH ER0                   ;@cwapp ;@preserve
    PUSH ER2                   ;@cwapp ;@preserve
    PUSH ER4                   ;@cwapp ;@preserve
    PUSH ER6                   ;@cwapp ;@preserve
    PUSH ER8                   ;@cwapp ;@preserve
    PUSH ER10                  ;@cwapp ;@preserve
    PUSH ER12                  ;@cwapp ;@preserve
    PUSH ER14                  ;@cwapp ;@preserve
    MOV R0, #(__cwapp_preserve_state & 0FFH)       ;@cwapp ;@preserve
    MOV R1, #((__cwapp_preserve_state >> 8) & 0FFH);@cwapp ;@preserve
    L R14, [ER0]               ;@cwapp ;@preserve
    CMP R14, #0                ;@cwapp ;@preserve
    BEQ __rst_done             ;@cwapp ;@preserve
    MOV ER2, ER0               ;@cwapp ;@preserve
    ADD ER2, #2                ;@cwapp ;@preserve
    MOV ER10, ER0              ;@cwapp ;@preserve
    ADD ER10, #34              ;@cwapp ;@preserve
    MOV R15, #0                ;@cwapp ;@preserve
__rst_one:                     ;@cwapp ;@preserve
    L R6, [ER2]                ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    L R7, [ER2]                ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    L R8, [ER2]                ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    L R9, [ER2]                ;@cwapp ;@preserve
    ADD ER2, #1                ;@cwapp ;@preserve
    MOV ER12, ER6              ;@cwapp ;@preserve
    ADD ER12, ER8              ;@cwapp ;@preserve
__rst_copy:                    ;@cwapp ;@preserve
    L R0, [ER10]               ;@cwapp ;@preserve
    ST R0, [ER6]               ;@cwapp ;@preserve
    ADD ER6, #1                ;@cwapp ;@preserve
    ADD ER10, #1               ;@cwapp ;@preserve
    CMP ER6, ER12              ;@cwapp ;@preserve
    BNE __rst_copy             ;@cwapp ;@preserve
    ADD R15, #1                ;@cwapp ;@preserve
    CMP R15, R14               ;@cwapp ;@preserve
    BLTS __rst_one             ;@cwapp ;@preserve
__rst_done:                    ;@cwapp ;@preserve
    POP ER14                   ;@cwapp ;@preserve
    POP ER12                   ;@cwapp ;@preserve
    POP ER10                   ;@cwapp ;@preserve
    POP ER8                    ;@cwapp ;@preserve
    POP ER6                    ;@cwapp ;@preserve
    POP ER4                    ;@cwapp ;@preserve
    POP ER2                    ;@cwapp ;@preserve
    POP ER0                    ;@cwapp ;@preserve
    RT                         ;@cwapp ;@preserve
