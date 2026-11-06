// asset_fallback.hpp - embedded copies of runtime.asm and the boot templates.
// Generated from compiler/ so wizcore still works when the data directory is
// missing (for example before an embedder calls set_data_dir()).
#pragma once
#include <string>

namespace wiz {
namespace embedded {
inline const char* runtime_asm() {
    return R"WIZASSET(; Build tags: a line ending in ";@2p" is kept only for 2-bitplane targets (cwii), ";@1p" only for
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
)WIZASSET";
}
inline const char* template_ML620906_asm() {
    return R"WIZASSET(; ML620906 boot, generated by tools/gen_template906.py from ML620Q906.asm + the 906 DCL.
; Symbol names were replaced by raw SFR addresses. Names the DCL does not define (inferred):
;   XTM0    = 0F00AH.0  FCON.0 = LOSCEN in the DCL; the 909 boot does the same thing as "ST 81H -> 0F00AH"
;   XTM1    = 0F00AH.1  FCON.1 = LOSCM in the DCL; the 909 boot clears 0F00AH.1 in the same spot
;   KIMASK  = 0F042H    libcw.h KeyboardInMask = 0xF042 (DCL calls it KIMOD); 909 boot writes 0 there
;   KOMASK  = 0F044H    libcw.h KeyboardOutMask = 0xF044 (DCL calls it KOCON); 909 boot writes 80H,0FFH there

type(ML620906)
model large

_$$SP	EQU	0F000h

dw	_$$SP
dw	_START
dw	_BRK
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT

_BRK:
	mov	psw,	#3
	brk

_INT:
	rti

_START:
	bl	_reset_lr
	mov	r0,	#0
	st	r0,	0F000H
	bl	_init_SFR
	mov	r0,	#0C8h
	mov	r1,	#5
	bl	_timer
	bl	_reset_SFR
	b	_entry

_reset_lr:
	rt

_init_SFR:
	push	lr
	mov	r0,	#31h
	st	r0,	0F00AH
	mov	r0,	#17h
	st	r0,	0F028H

	mov	r0,	#32h
_$init_clock:
	add	r0,	#-1h
	bne	_$init_clock
	lea	0F010H
	mov	er0,	#20h
	st	er0,	[EA+]
	st	r1,	[EA]
	mov	r0,	#7h
	st	r0,	0F03Dh
	mov	r0,	#50h
	mov	r1,	#0
	bl	_timer
	bl	_init_screen
	bl	_init_port1
	bl	_init_port0
	bl	_init_p00
	mov	er0,	#0
	st	r0,	0F042H
	st	er0,	0F046H
	pop	pc

_timer:
	sb	0F00AH.0
	rb	0F00AH.1
	rb	0F028H.3
	di
	mov	er2,	#1
	st	er2,	0F024H
	mov	er2,	#0
	st	er2,	0F022H
	st	er2,	0F014H
	st	r2,	0F016H
	sb	0F010H.5
	st	er0,	0F020H
	mov	r0,	#1
	st	r0,	0F025H
_stop:
	mov	r0,	#50h
	mov	r1,	#0A0h
	st	r0,	0F008H
	st	r1,	0F008H
	mov	r0,	#2
	st	r0,	0F009H
	nop
	nop
	rt

_init_screen:
	mov	r0,	#4
	st	r0,	0F030h
	mov	r0,	#7
	st	r0,	0F033h
	mov	r0,	#9
	st	r0,	0F034h
	mov	r0,	#17h
	st	r0,	0F035h
	mov	er0,	#1
	st	r0,	0F036h
	st	r1,	0F039h
	mov	r0,	#17h
	st	r0,	0F031h
	mov	r0,	#12h
	st	r0,	0F032h
	rt

_init_port1:
	lea	0F220H
	mov	r0,	#0
	mov	r1,	#7Fh
	st	er0,	[EA+]
	st	er0,	[EA+]
	st	r0,	[EA]
	rt

_init_port0:
	lea	0F048H
	mov	er0,	#0
	mov	er2,	#7
	st	er0,	[EA+]
	st	er2,	[EA+]
	st	r2,	[EA]
	rt

_init_p00:
	mov	r0,	#0
	st	r0,	0F041H
	mov	r0,	#80h
	mov	r1,	#0FFh
	st	er0,	0F044H
	rt

_reset_SFR:
	mov	r0,	#1
	st	r0,	0F00AH
	mov	r0,	#4
	st	r0,	0F030h
	mov	r0,	#3
	st	r0,	0F033h
	mov	r0,	#9
	st	r0,	0F034h
	mov	r0,	#17h
	st	r0,	0F035h
	mov	er0,	#1
	st	r0,	0F036h
	st	r1,	0F039h
	mov	r0,	#15h
	st	r0,	0F031h
	rt

_entry:
	;start your program here
	b _main
)WIZASSET";
}
inline const char* template_ML620909_asm() {
    return R"WIZASSET(type(ML620909)
model large
romwindow 0, 08fffH
extrn   code: _main
public  $$start_up
_$$SP	EQU	0F000h

dw	_$$SP
dw	$$start_up
dw	_BRK
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT
dw	_INT

_BRK:
	mov	psw,	#3
	brk

_INT:
	rti

_060EC:
	PUSH LR
	MOV ER2, ER0
	TB 0F00AH.1
	BNE _$L074
_$L00A:
	DI
	MOV ER0, #01H
	ST ER0, 0F024H
	MOV ER0, #00H
	ST ER0, 0F022H
	ST ER2, 0F020H
	MOV R0, #01H
	ST R0, 0F025H
	MOV R0, #00H
	ST R0, 0F014H
	ST R0, 0F015H
	MOV R2, #50H
	MOV R3, #0A0H
	ST R2, 0F008H
	ST R3, 0F008H
	MOV R0, #02H
	ST R0, 0F009H
	NOP
	NOP
_$L044:
	L R0, 0F042H
	BNE _$L072
_$L04A:
	MOV ER0, #0FH
	ST ER0, 0F024H
	MOV ER0, #00H
	ST ER0, 0F022H
	MOV R0, #9EH
	MOV R1, #07H
	ST ER0, 0F020H
	MOV R0, #01H
	ST R0, 0F025H
	MOV R0, #00H
	ST R0, 0F014H
	ST R0, 0F015H
	EI
_$L072:
	POP PC
_$L074:
	RB 0F00AH.1
	BAL _$L00A

$$start_up:
	MOV R0, #00H
	ST R0, 0F000H
	B _2F060

_2F060:
	L R0, 0F058H
	MOV R4, R0
	BL _3DEDE
	MOV R0, #40H
	MOV R1, #06H
	BL _060EC
	MOV R0, #81H
	ST R0, 0F00AH
	MOV R0, #04H
	ST R0, 0F030H
	MOV R0, #03H
	ST R0, 0F033H
	MOV R0, #06H
	ST R0, 0F034H
	MOV R0, #17H
	ST R0, 0F035H
	MOV R0, #08H
	ST R0, 0F036H
	MOV R0, #00H
	ST R0, 0F039H
	MOV R0, #55H
	ST R0, 0F031H
	BL _entry


_3DEDE:
	PUSH LR
	MOV R0, #31H
	ST R0, 0F00AH
	MOV R0, #0F7H
	ST R0, 0F028H
	MOV R0, #32H
_$L010:
	ADD R0, #-1H
	CMP R0, #00H
	BNE _$L010
	MOV R0, #22H
	ST R0, 0F010H
	MOV R0, #00H
	ST R0, 0F011H
	ST R0, 0F012H
	MOV R0, #03H
	ST R0, 0F018H
	MOV R0, #00H
	ST R0, 0F058H
	MOV R0, #00H
	ST R0, 0F042H
	MOV R0, #07H
	ST R0, 0F03DH
	MOV R0, #0C8H
	MOV R1, #00H
_$L02E:
	BL _060EC
	MOV R0, #04H
	ST R0, 0F030H
	MOV R0, #07H
	ST R0, 0F033H
	MOV R0, #06H
	ST R0, 0F034H
	MOV R0, #17H
	ST R0, 0F035H
	MOV R0, #08H
	ST R0, 0F036H
	MOV R0, #00H
	ST R0, 0F039H
	MOV R0, #57H
	ST R0, 0F031H
	MOV R0, #12H
	ST R0, 0F032H
	MOV R0, #00H
	ST R0, 0F220H
	MOV R0, #7FH
	ST R0, 0F221H
	MOV R0, #00H
	ST R0, 0F222H
	MOV R0, #7FH
	ST R0, 0F223H
	MOV R0, #00H
	ST R0, 0F224H
	ST R0, 0F225H
	MOV R0, #00H
	ST R0, 0F048H
	ST R0, 0F049H
	MOV R0, #07H
	ST R0, 0F04AH
	MOV R0, #00H
	ST R0, 0F04BH
	MOV R0, #07H
	ST R0, 0F04CH
	MOV R0, #00H
	ST R0, 0F04EH
	MOV R0, #00H
	ST R0, 0F041H
	MOV R0, #80H
	ST R0, 0F044H
	MOV R0, #0FFH
	ST R0, 0F045H
	MOV R2, #00H
	ST R2, 0F046H
	POP PC

_entry:
	b _main
	;start your program here
)WIZASSET";
}
inline const char* template_cwapp_asm() {
    return R"WIZASSET(; cwapp: a Wiz program that is patched INTO a fx-580VN X (ML620906, 256 KB) firmware ROM.
; It lives in the free flash at 3:0E40 (ROM offset 30E40h) and is entered from the firmware's own menu.
; Written from reading the ROM with a disassembler - NOT yet run on a real calculator.
;
; Hook: patch_menu.py overwrites the firmware instructions at 2:5E1E
;           CMP R8, 03 / BEQ 2:5E88            (inside set_mode, 2:5D82; R8 = mode id, from menu table 0x7FF8 +2)
;       with  B 3:0E40.  _TRAMP below handles our mode id 0E0h and otherwise replays the replaced instructions.

type(ML620906)
model large

_ROMSEG EQU 3

    ORG 30E40h

_TRAMP:
    mov r0, #0
    st r0, 0E7F4H
    cmp r8, #0E0h
    bne _t_old
    b _APP
_t_old:
    cmp r8, #3
    bne _t_back
    b 25E88h
_t_back:
    b 25E22h

; Boot dispatcher entry (2:9448 / 0:29448): a power-on/reboot reaches the
; mode dispatcher without calling set_mode.  Only the app's mode id restarts
; the app; every other id (including the 0x0D menu id) uses the stock menu
; function, so MENU still returns to the menu.
_BOOT_DISPATCH:
    cmp r0, #0E0h
    beq _BOOT_APP
    b 2E234h
_BOOT_APP:
    mov r0, #1
    st r0, 0E7F4H
    b _APP

_APP:
    ; Save the caller's stack, registers, PSW and LR so MENU / exit() can
    ; return through the firmware path instead of resetting the calculator.
    ; The copy lives at 0xE5D3..0xE7FF (below the runtime heap at 0xE820).
    push lr
    push psw
    push er0
    push er2
    push er4
    push er6
    push er8
    push er10
    push er12
    push er14
    mov er0, sp
    st r0, 0E5D3h
    st r1, 0E5D4h
    mov r4, #0E5h
    mov r5, #0EDh
    cmp er0, er4
    blts _save_fail
    mov r2, #0D5h
    mov r3, #0E5h
    mov r4, #0
    mov r5, #0F0h
_save_loop:
    cmp er0, er4
    beq _save_done
    l r6, [er0]
    st r6, [er2]
    add er0, #1
    add er2, #1
    b _save_loop
_save_fail:
    mov r0, #0
    st r0, 0E5D3h
    st r0, 0E5D4h
_save_done:
    di
    mov r0, #0
    mov r1, #0F0h
    mov sp, er0
    mov r0, #0
    st r0, 0F041H
    mov er0, #0
    st er0, 0F044H       ; KOMASK = 0: all key columns enabled
    st r0, 0F042H        ; KIMASK = 0
    st er0, 0F046H       ; KO = 0
    b _main

; exit(): restore the caller and return to the firmware menu.  __cwapp_return
; is generated by runtime_for() for the legacy (set_mode) or extra (mode
; routine) caller.  The reset path is only a last-resort fallback when the
; saved stack did not fit in the cwapp save area.
__exit:
    b __cwapp_return

__cwapp_reset:
    mov r0, #0
    st r0, 0E7F4H
    di
    mov r0, #0
    mov r1, #0F0h
    mov sp, er0
    ; reset key SFRs the firmware's reset handler expects at power-on state
    mov r0, #17h
    st r0, 0F028H        ; BLKCON0: re-enable peripheral clocks
    mov r0, #0
    st r0, 0F042H        ; KIMASKn = 0 (block key input to firmware)
    st er0, 0F046H       ; KOMASKn = 0
    b 0A1CEh
)WIZASSET";
}
inline const char* template_fwnx_asm() {
    return R"WIZASSET(; template_fwnx.asm - empty boot template for full firmware ROM image (fwnx target)
; The assembled output starts at address 0 and covers the full 256KB.
; No boot code, no runtime appended.

ORG 0)WIZASSET";
}
}  // namespace embedded

inline const std::string& embedded_asset(const std::string& name) {
    static const std::string empty;
    static const std::string runtime = embedded::runtime_asm();
    static const std::string t906 = embedded::template_ML620906_asm();
    static const std::string t909 = embedded::template_ML620909_asm();
    static const std::string tcwapp = embedded::template_cwapp_asm();
    static const std::string tfwnx = embedded::template_fwnx_asm();
    if (name == "runtime.asm") return runtime;
    if (name == "template_ML620906.asm") return t906;
    if (name == "template_ML620909.asm") return t909;
    if (name == "template_cwapp.asm") return tcwapp;
    if (name == "template_fwnx.asm") return tfwnx;
    return empty;
}

}  // namespace wiz