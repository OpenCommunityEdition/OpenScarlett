// SPDX-License-Identifier: MIT
// Wave32 code that uses 64-bit forms on the EXEC pair, as the console's ray-traversal code does (s_or_saveexec_b64 sN, -1 around
// sections every lane runs). The saveexec_b32 decides wave32: a compare then writes one mask register (s9 keeps 0x1234; read as
// wave64 code it would get the mask's high half), and the 64-bit save and restore of EXEC work.
;! lanes 0x1234 + 7
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
s_waitcnt lgkmcnt(0)
s_mov_b32 s9, 0x1234
v_mov_b32 v2, 1
v_mov_b32 v1, 0
v_cmp_eq_u32_e64 s8, v2, v2
s_and_saveexec_b32 s10, s8
s_or_saveexec_b64 s[12:13], -1
v_mov_b32 v1, s9
s_mov_b64 exec, s[12:13]
s_mov_b32 exec_lo, s10
v_add_nc_u32 v1, 7, v1
buffer_store_dword v1, v0, s[4:7], 0 offen
s_endpgm
