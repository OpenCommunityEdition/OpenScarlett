// SPDX-License-Identifier: MIT
// wave64: ds_bpermute_b32 reads within the caller's 32-lane half: lane L reads lane
// (L & 32) | (((L + 5) * 4 >> 2) & 31). The data is each lane's own index (v_mbcnt).
// v_readlane_b32 takes the lane number modulo 64 (lane 70 = lane 6).
;! wave64
;! lanes (L & 32) | ((L + 5) & 31)
;! lanes+64 6
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
v_add_nc_u32 v2, 5, v3
v_lshlrev_b32 v2, 2, v2
ds_bpermute_b32 v1, v2, v3
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen
s_mov_b32 s20, 70
v_readlane_b32 s21, v3, s20
v_mov_b32 v1, s21
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
s_endpgm
