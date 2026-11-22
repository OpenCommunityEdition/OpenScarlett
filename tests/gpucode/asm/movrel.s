// SPDX-License-Identifier: MIT
// v_movrels_b32 / v_movreld_b32: VGPRs indexed by M0. v10..v12 hold L+100, L+200, L+300.
// With M0 = 1, v_movrels_b32 v1, v10 reads v11 (L+200); with M0 = 2, v_movreld_b32 v10, v3
// writes v12 (= L), and v_movrelsd_b32 v10, v10 with M0 = 0 changes nothing.
;! wave64
;! lanes L + 200
;! lanes+64 L
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
s_waitcnt lgkmcnt(0)
v_mbcnt_lo_u32_b32 v3, -1, 0
v_mbcnt_hi_u32_b32 v3, -1, v3
v_add_nc_u32 v10, 100, v3
v_add_nc_u32 v11, 200, v3
v_add_nc_u32 v12, 0x12c, v3
s_mov_b32 m0, 1
v_movrels_b32 v1, v10
buffer_store_dword v1, v0, s[4:7], 0 offen
s_mov_b32 m0, 2
v_movreld_b32 v10, v3
s_mov_b32 m0, 0
v_movrelsd_b32 v10, v10
buffer_store_dword v12, v0, s[4:7], 0 offen offset:256
s_endpgm
