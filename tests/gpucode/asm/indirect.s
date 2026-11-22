// SPDX-License-Identifier: MIT
// Indirect call and return: s_getpc_b64 + offset gives the function's address (0x58), s_swappc_b64
// jumps there and leaves the return address (0x3c) in s[30:31], the function adds 0x60 to v1 and
// returns with s_setpc_b64. Results: v1 = 0x11 + 0x60, then the return address.
;! wave64
;! lanes 0x71
;! lanes+64 0x3c
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
v_mov_b32 v1, 0x11
s_getpc_b64 s[20:21]
s_add_u32 s20, s20, 0x28
s_addc_u32 s21, s21, 0
s_swappc_b64 s[30:31], s[20:21]
s_waitcnt vmcnt(0) lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen
v_mov_b32 v1, s30
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
s_endpgm
v_add_nc_u32 v1, 0x60, v1
s_setpc_b64 s[30:31]
