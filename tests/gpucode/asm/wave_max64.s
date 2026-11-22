// SPDX-License-Identifier: MIT
// wave64 maximum as compilers emit it: DPP8 steps (groups of 8), row_mirror with bound_ctrl
// (rows of 16), v_permlanex16_b32 (the other row of each 32-lane half), then v_readlane_b32 of
// lane 32 and lane 0. Lane values: (L * 37) % 64 + 100: the 32-lane maxima are 163 (lane 19) and
// 162, the wave maximum 163. A second run keeps lane 0..31 small to tell the halves apart:
// values L + 1 for the low half, 200 + (L & 7) for the high half.
;! wave64
;! lanes (163 if L < 32 else 162)
;! lanes+64 163
;! lanes+128 (32 if L < 32 else 207)
;! lanes+192 207
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
v_mbcnt_lo_u32_b32 v3, -1, 0
v_mbcnt_hi_u32_b32 v3, -1, v3
v_mul_u32_u24 v5, 37, v3
v_and_b32 v5, 63, v5
v_add_nc_u32 v5, 100, v5
s_or_saveexec_b64 s[10:11], -1
v_cndmask_b32_e64 v2, 0, v5, s[10:11]
v_max_u32_dpp v2, v2, v2 dpp8:[1,0,3,2,5,4,7,6]
v_max_u32_dpp v2, v2, v2 dpp8:[2,2,0,0,6,6,4,4]
v_max_u32_dpp v2, v2, v2 dpp8:[4,4,4,4,0,0,0,0]
v_max_u32_dpp v2, v2, v2 row_mirror row_mask:0xf bank_mask:0xf bound_ctrl:1
v_permlanex16_b32 v6, v2, 0, 0
v_max_u32 v2, v6, v2
buffer_store_dword v2, v0, s[4:7], 0 offen
v_readlane_b32 s12, v2, 32
v_max_u32 v2, s12, v2
v_readlane_b32 s12, v2, 0
s_mov_b64 exec, s[10:11]
v_mov_b32 v1, s12
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
v_add_nc_u32 v5, 1, v3
v_and_b32 v7, 7, v3
v_add_nc_u32 v7, 200, v7
v_cmp_gt_u32 vcc, 32, v3
v_cndmask_b32 v5, v7, v5, vcc
v_mov_b32 v2, v5
v_max_u32_dpp v2, v2, v2 dpp8:[1,0,3,2,5,4,7,6]
v_max_u32_dpp v2, v2, v2 dpp8:[2,2,0,0,6,6,4,4]
v_max_u32_dpp v2, v2, v2 dpp8:[4,4,4,4,0,0,0,0]
v_max_u32_dpp v2, v2, v2 row_mirror row_mask:0xf bank_mask:0xf bound_ctrl:1
v_permlanex16_b32 v6, v2, 0, 0
v_max_u32 v2, v6, v2
buffer_store_dword v2, v0, s[4:7], 0 offen offset:512
v_readlane_b32 s12, v2, 32
v_max_u32 v2, s12, v2
v_readlane_b32 s12, v2, 0
v_mov_b32 v1, s12
buffer_store_dword v1, v0, s[4:7], 0 offen offset:768
s_endpgm
