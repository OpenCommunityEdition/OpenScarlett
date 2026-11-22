// SPDX-License-Identifier: MIT
// VALU edges: v_perm_b32 selector byte 12 gives 0x00 and 13 or more 0xff (selector 0x0c0d0e0f -> 0x00ffffff); v_ffbh_u32(0) is
// 0xffffffff; v_bfe_u32 with width 0 is 0.
;! lanes 0x00ffffff
;! lanes+64 0xffffffff
;! lanes+128 0
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
s_waitcnt lgkmcnt(0)
v_mov_b32 v2, 0x0c0d0e0f
v_mov_b32 v3, 0x11223344
v_perm_b32 v1, v3, v3, v2
buffer_store_dword v1, v0, s[4:7], 0 offen
v_mov_b32 v2, 0
v_ffbh_u32 v1, v2
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
v_mov_b32 v2, 0xffffffff
v_bfe_u32 v1, v2, 4, 0
buffer_store_dword v1, v0, s[4:7], 0 offen offset:512
s_endpgm
