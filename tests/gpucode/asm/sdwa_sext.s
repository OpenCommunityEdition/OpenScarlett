// SPDX-License-Identifier: MIT
// SDWA dst_unused:UNUSED_SEXT sign-extends the written field upwards; bits below it are zero.
// 0xab written to byte 1 is negative: 0xffffab00. Same for the WORD_0 case with 0x8001: 0xffff8001.
;! lanes 0xffffab00
;! lanes+64 0xffff8001
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
s_waitcnt lgkmcnt(0)
v_mov_b32 v2, 0x1234f0ab
v_mov_b32_sdwa v1, v2 dst_sel:BYTE_1 dst_unused:UNUSED_SEXT src0_sel:BYTE_0
buffer_store_dword v1, v0, s[4:7], 0 offen
v_mov_b32 v2, 0x77778001
v_mov_b32_sdwa v3, v2 dst_sel:WORD_0 dst_unused:UNUSED_SEXT src0_sel:WORD_0
buffer_store_dword v3, v0, s[4:7], 0 offen offset:256
s_endpgm
