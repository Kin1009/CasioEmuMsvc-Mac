#pragma once

// rop_data.hpp - gadget/label tables for the ROP backend.
// Derived from CasioC hdcompiler/{580vnx,880btg}/{gadgets,labels}.

static const char ROP_DB_580[] = R"ROPDB(
09fa8 setlr
308d0 setlr_pc
0a05c DI,RT
23f9e calc_checksum_set_f004
23fa6 calc_checksum_no_set_f004
23fad calc_checksum_0
23fc0 calc_checksum_1
23fd4 calc_checksum_2
23fea calc_checksum_3
23f21 pr_checksum
09ca0 [er8]+=er2,pop xr8
2702e sp = er14,pop er14,rt
12da6 sp = er14,pop qr8
23f82 sp = er14,pop qr8,pop qr0
20d60 sp = er14,pop er14
21f74 sp = er6,pop er8
09820 sp = er14,pop xr12
13aac sp = er14,pop qr8,pop er6
0ac34 er14 = sp,rt
0a8a8 nop
13b66 er0+=er0,er0+=er2,er8=[er0]
13b9a er0+=er0,er2+=er0,er0=[er2]
1ec0c pop ea
27030 pop er14,rt
13332 pop er0,rt
18974 pop er2
250a6 pop er4
0a8a6 pop er8
16eca pop er12,rt
17bda pop qr0
18932 launcher_copy
12da8 pop qr8
177da pop r0
1844a pop r8
17b34 pop xr0
1d040 pop xr4
183a6 pop xr8
0d516 pop er10
0d04c pop r12
12602 pop er0
21636 pop er12
15c78 pop er14
13ab0 pop er6
1dcf6 pop er6,rt
20730 pop xr12
16d88 pop er4,rt
16d7c pop er8,rt
17072 pop qr0,rt
14c50 pop qr8,rt
15fc0 pop r4
16210 pop r4,rt
14c98 pop r9
10e5a pop xr12,rt
1d972 pop xr4,rt
0e5e2 pop xr8,rt
093b8 pop er2,pop er8,er0=er2,rt
1893a pop er0,pop er4
16754 pop er4,pop er8
201f6 pop er4,pop er12,pop r9
14bd8 er0+=er4,rt
13b80 er4+=er0,r8 = r8,rt
21ba8 er0+=er8,rt
21bae er2+=er8,rt
2fa0c er0+=er2,rt
16078 er0+=1,rt
09ce4 r0+=1,rt
20838 and r0,0f
24008 er6 = er0,er0 = er8,pop qr8
16170 er8 = er0
0ea70 er2 = er0,er0 = er2,pop er8,rt
14bd6 er2 = er0,er0+=er4,rt
0bf30 er0 = er2,rt
2470a er0 = er4,pop er4
2f786 er0 = er8,pop er8,rt
15386 er0 = er8
1edea er0 = er6,pop er8,pop xr4
28c86 er2 = er0,r0 = r4,r1 = 0,pop xr4,rt
2ffc0 r0 = r5,pop er4
1abb0 r0 = r2 = 0
1abb2 r0 = r2
15fde r2 = r0,pop er0
164e4 r2 = r0,pop r6,pop er12
177b4 r2 = 0,r7 = 4
1ca8a r0 = 0
1DD54 r0 = 0,rt
1DD58 r0 = 1,rt
18978 r0 = 0,pop er2
2ac30 r1 = 0,rt
16df2 r5 = 0,rt
17b32 er14 = er0,pop xr0
0ac7c er0 = er12,pop er12,rt
13916 er10 = er2,rt
20b52 er0 = er10,pop xr8
1e60c er0 = 1,rt
180d2 er2 = 0,er4 = 0,er6 = 0,er8 = 1,rt
09caa er2 = 0,r0 = 2,[er8]=er2,pop xr8
0a88a er2 = 1,r0 = r2,rt
09c9e r0 = 0,[er8]+=er2,pop xr8
27e7c r2 = 1,r0 = r2,pop er4,pop er8,rt
27e92 r0 = r1,rt
16d9a [er2]=er0,r2 = 0,pop er4,rt
139d8 [er0]=er2,rt
208b2 [er0]=r2,rt
203D2 [er0]=r2
176d4 [er2]=r0,r2 = 0
09ca4 [er8]=er2,pop xr8
13330 [er4]=er0,pop er0,rt
1a588 [ea]=qr0
22226 [er12]=er14,pop xr4,pop qr8
1332a [er4]+=1,rt
13336 [er4]-=1,rt
16d7a er4=[er8],pop er8,rt
13b9e er0=[er2],r2 = 9,rt
13b6a er8=[er0],rt
09e4a r0=[er2]
18274 r0=[er0]
298dc er0=[er0],pop xr8,rt
09fdc r0=[ea],rt
21f72 sp=[er8],pop er8
1c2c0 qr0=[ea],lea D002H,[ea]=qr0
1c64a er6 = [ea]
09e8a er0-=er2,rt
29a40 er0-=er12,pop er8,pop er12,rt
09e98 r0-=1,rt
0ac5a r0-=r8,pop er8,rt
122d4 or r0,r1
1a5c2 or qr0,qr8
20830 r0 >> 4,rt
1bb28 qr0 >> 4,rt
1bb5c r0 << 4,rt
1bb70 r1 << 4,rt
1bb5a er0 << 4,rt
1bb56 xr0 << 4,rt
1bb4e qr0 << 4,rt
0c790 er0 - er2_gt,r0 = 0 |r0 = 1,rt
09ae6 er0 - er2_eq,r0 = 1,rt
0c7a8 er2 - er0_gt,r0 = 0 |r0 = 1,rt
2abda er0 - er2_le,er0 = er2,rt
297e2 er8 - er0_lt,pop xr8
091ec r0 - 0_lt,rt
091f4 r1 - 0_lt,rt
14bd4 er0*=r2,er2 = er0,er0+=er4,rt
14fca er0*=r2,er0+=er6,er10 = er0,rt
28C54 er0/=r2,rt
33030 brk
0d71c [ea]+=1,r0=3
2c81e [ea]-=1,pop xr4
24836 B LEAVE
09450 BL memcpy,pop er0
203CC BL strcpy
203E0 BL strcat
09D3A BL memset,pop er2
23CAC BL delay,pop xr0
20C40 BL line_print
23F62 BL printline
24004 BL hex_byte,er6 = er0,er0 = er8,pop qr8
20A50 BL smart_strcpy,pop er8
0A052 BL zero_KO
28AC2 BL line_draw
09AD4 BL render.ddd4
09E96 [ea+]=r0,r0-=1,bne
09C20 cmp_ea
17922 calc_func
16082 cvt_hex_1
160f8 cvt_hex_2
10f20 memcpy_length_rn/8
1700e memcpy_length_rn/2, pop qr0
13324 pop pc(1:3324)
0E5C8 strcpy
2EDA0 strcat
26FFA memcpy
0A8AA memmove
29A2C strlen
1EB94 memset
203C2 smart_strcpy
203D6 smart_strcat
203B8 smart_strlen_n
2086C basen_base_print
23DCC smallprint
08F7C line_print.col_0
08F80 line_print
23DC8 printline
09470 render.e3d4
0947E render.ddd4
09848 render_bitmap
09D34 memzero
26086 reset_routine
24BD6 get_string_constant
08C0C fill_screen
08F02 str_decompress_print
09F3C delay
0A1CE _start
293D8 main
09440 buf1_to_buf2
09458 buf2_to_buf1
2AB26 byte_strlen_n
23C4C diagnostic_mode
0ACB6 f_0ACB6
0AD92 diagnostic_wait_key
0E826 check_any_key_pressed__
23CB4 diagnostic
24344 diag_scr_888_ws
23DD6 diag_scr_fill_ws
23DDA waitshift_striped
23DDE waitshift
23DE4 diag_scr_ckb1_ws
23E40 diag_scr_ckb2_ws
23E6A diag_scr_version
23E92 diag_print_ver
23EEE diag_checksum
23F8A store_reg_to_stack
23EC8 diag_print_pd
23F9A diag_calc_checksum
09A24 pd_value
24010 hex_byte
16EAE byte_hex
09938 hex_to_dec
2541A diag_serial_num
10E5E get_serial_num
23D5A diag_check_key
2641A diag_contrast
2C58A diag_8_keytest
1F24C getscancode
2F5E8 getkey
2F5E8 getkeycode
29892 cvt_key
0A0E0 setsfr
19808 byte_to_bcd
19832 bcd_to_byte
08E62 line_draw
091FC pixel_draw
090D2 draw_glyph
2EDC8 line_print__call__
0996C str_decompress_print__call__
099C2 str_decompress_print__call1__
0904E char_print_1byte
09056 char_print
09238 char_get_14
09318 char_get_l14
0A16A zero_KO
09DE6 assign_var
08C60 buffer_clear
0AC30 ENTER
0AC38 LEAVE
224DE LEAVE1
22D66 LEAVE2
13D92 LEAVE3
2110A LEAVE4
1652C num_add
16538 num_sub
16544 num_mul
16550 num_div
1D24A num_add_1
1D236 num_sub_1
1D272 num_mul_1
1D25E num_div_1
20C46 num_output_print
1D950 num_fromdigit
1DC2E num_frombyte
1D898 num_invalid__
1DBE2 num_trunc__
1DAF0 num_mulxp__
1391A num_random
13A20 num_randint
139D2 num_normalize
1DC8A num_to_byte
279B6 num_to_str
1DCE4 num_cpy
1D902 num_cmp
16844 num_sin
1684E num_cos
168E4 num_tan
2AD4C display_menu
093D2 draw_byte
083DA d_adrcvtkey
0D000 d_reg0
0D009 d_reg0.9
0D110 d_modifiers
0D111 d_mode
0D112 d_submode
0D11A d_num_format
0D11B d_num_format_i
0D11D d_angle_unit
0D138 d_draw_mode
0D180 d_input_range
0D318 d_unstable_char
0D31A d_var_m
0D324 d_var_ans
0D32E d_var_a
0D338 d_var_b
0D342 d_var_c
0D34C d_var_d
0D356 d_var_e
0D360 d_var_f
0D36A d_var_x
0D374 d_var_y
0D37E d_var_preans
0D388 d_var_z
0D392 d_calc_history
0D139 d_current_screen_buffer
0DDD4 d_screen_buffer
0D137 d_font_size
0D113 d_cursor_noflash
0DBD0 d_magic_string
203cc bl_strcpy
)ROPDB";

static const char ROP_DB_880[] = R"ROPDB(
17B7E pop qr0
13956 pop qr8
16438 pop xr0
17136 pop xr4
23B86 pop xr8
15E92 pop xr12
18958 pop er0
20692 pop er2
19868 pop er4
164BE pop er6
42F30 pop er8
4525C pop er10
2088A pop er12
1086C pop er14
19C58 pop r0
208C8 pop r1
16160 pop r4
166F0 pop r6,pop er12
16F9E pop r8
201FA pop r9
395D8 pop r12
2A5EE pop ea
33920 pop pc
09CCE setsfr
06E2E setlr
06E82 di,rt
06E72 NOP
83030 brk
351B0 hex_byte,pop xr8
13452 [er4]+=1
1345E [er4]-=1
090CA [er10+c] += er0,sp=er14,pop xr4,pop qr8
43154 [er10]+=er0,r0=0,pop xr8
13454 er0 = [er4],er0+=1,[er4] = er0,pop er0,rt
0C1A2 r1=0,[er8]+=er2,er2+=er0,st er2,[er8],r0=1,pop er4,er8,er12,rt
13D70 er0 = [er2],r2 = 9,rt
13D3C er8 = [er0],rt
17006 er4 = [er8],pop er8,rt
39B0C er0 = [er0],pop xr8,rt
1934E [er0] = r2,r0=0
1681E [er2] = r0,r0=0
0E3C2 [er10] = er0,r0 = 0,pop er10
13B92 [er0]=er2,rt
17026 [er2] = er0,r2 = 0,pop er4
13458 [er4] = er0,pop er0,rt
1557A er0 = er8
1637A er8 = er0
1807E er14 = er0,pop xr0
13D62 er2=er0,er0+=er4,rt
37106 er2=er0,er0=er2
370FE er0=er2
0B052 r0=r1,rt
151BE er10=er0,rt
49934 er0+=er2,rt
20F2E er2+=er8,rt
20F28 er0+=er8,rt
13D52 er4+=er0,r8=r8,rt
0B0B6 er2+=2,er0=er2,rt
0B0BA er2+=1,er0=er2,rt
13D6E er2+=er0,er0=[er2],r2=9,rt
13D64 er0+=er4,rt
091C2 r0-=r2,rt
13D60 er0*=r2,er2 = er0,er0+=er4,rt
380FE er0/=r2,rt
13D38 jumptablesub
19910 er0 - er2_gt,r0 = 0,pop er4
41B70 er2 - er4_beq,r0 = 0,pop er12,pop xr4,pop xr8,sp = er14,pop er14
07D26 er0 = 0,r4 = 1,r0 = r4,pop er4
2EF96 r3 > bf,r0 = 00,pop er8,rt
09036 r0 < 3f,r0 = 01,pop er8,rt
325E6 r0 & 0f,rt
123DC r0 | r1
1C9E4 r1 << 4
1C9C8 r0 << 4
21342 sp=[er8],pop er8
21344 sp=er6,pop er8
199EC sp=er4,sp+=46,pop xr4,pop qr8
29852 sp=er14,pop er14
4335C B leave
14DE8 BL memcpy,pop er4
0A470 BL render.ca54
4B68C er2_switch
0B0AE check_Fxxx
24172 strcpy
466F0 strcat
34AC2 memcpy
39D64 memzero
2A576 memset
3A2D4 strlen
06F8A memmove
349F8 smart_strcpy
2F7D8 smart_strcat
41B82 smart_strlen_n
1EFBA smart_memcpy
23D40 strcpy_rn
335EA memcpy_rn
47C9C getkey
31AFA getkeycode
494DC getscancode
39AC4 cvt_key
06E9E zero_KO
48302 zero_KI
18F1A buffer_clear.ca54
18F36 buffer_clear.d654
42994 render
46334 render.ca54
07E60 render_bitmap
1FABC print_line
395A6 line_print
1FAC2 smallprint
1FAC8 scaleprint
1EC60 draw_byte
3A1D6 fill_draw
09CFA line_draw
45914 pixel_draw
0A41C frame_draw
0919E ENTER
091A6 LEAVE
06DC0 delay
06F04 _start
39442 main
240DE basen_base_print
09D7E diagnostic_wait_key
395BC diag_scr_fill_ws
23136 diag_calc_checksum
2B8DC diag_waitkey__
332B2 diag_contrast
0A476 diagnostic
2DDE4 mode_diagnostic
3C2CA fill_screen
3AF54 shutdown
23126 store_reg_to_stack
24F32 diag_checksum
30F26 diag_serial_num
4B694 diag_scr_ckb_ws
24CAA check_any_key_pressed__
17148 byte_hex
05C9E d_base_N_keycode
)ROPDB";
