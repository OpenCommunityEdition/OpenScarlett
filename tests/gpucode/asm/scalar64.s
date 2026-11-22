// SPDX-License-Identifier: MIT
// 64-bit scalar ops: s_bfm_b64 masks (width 40 at offset 4); s_lshl_b64 takes the count mod 64
// (64 -> 0, 65 -> 1); s_ashr_i64 of 0x8000000000000000 by 63 gives all ones.
;! lanes 0xfffffff0
;! lanes+64 0x00000fff
;! lanes+128 0x89abcdef
;! lanes+192 0x13579bde
;! lanes+256 0xffffffff
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
s_waitcnt lgkmcnt(0)
s_bfm_b64 s[10:11], 40, 4
v_mov_b32 v1, s10
buffer_store_dword v1, v0, s[4:7], 0 offen
v_mov_b32 v1, s11
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
s_mov_b32 s12, 0x89abcdef
s_mov_b32 s13, 0x09abcdef
s_lshl_b64 s[14:15], s[12:13], 64
v_mov_b32 v1, s14
buffer_store_dword v1, v0, s[4:7], 0 offen offset:512
s_lshl_b64 s[14:15], s[12:13], 65
v_mov_b32 v1, s14
buffer_store_dword v1, v0, s[4:7], 0 offen offset:768
s_mov_b32 s16, 0
s_mov_b32 s17, 0x80000000
s_ashr_i64 s[18:19], s[16:17], 63
v_mov_b32 v1, s19
buffer_store_dword v1, v0, s[4:7], 0 offen offset:1024
s_endpgm
