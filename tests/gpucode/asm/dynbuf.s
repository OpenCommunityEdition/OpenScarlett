// SPDX-License-Identifier: MIT
// Run-time descriptors and pointers (global-memory test mode: memory = the t1 buffer).
// A raw buffer descriptor built in SGPRs (base 0x100, stride 8): buffer_load_dword offen with
// offset:4 reads 0x104; idxen with index 2 reads 0x100 + 2 * 8 = 0x110. s_load_dword through a
// pointer from v_readfirstlane (unknown when translating) reads 0x204.
;! gmtest
;! mem 0x100 0x0 0x1234 0x0 0x0 0x5678
;! mem 0x200 0x0 0x9abc
;! lanes 0x1234
;! lanes+64 0x5678
;! lanes+128 0x9abc
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
v_mov_b32 v4, 0x100
v_readfirstlane_b32 s20, v4
v_mov_b32 v4, 0x80000
v_readfirstlane_b32 s21, v4
s_mov_b32 s22, -1
s_mov_b32 s23, 0
v_mov_b32 v2, 0
v_mov_b32 v3, 2
buffer_load_dword v1, v2, s[20:23], 0 offen offset:4
s_waitcnt vmcnt(0) lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen
buffer_load_dword v1, v3, s[20:23], 0 idxen
s_waitcnt vmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
v_mov_b32 v4, 0x200
v_readfirstlane_b32 s24, v4
s_mov_b32 s25, 0
s_load_dword s26, s[24:25], 0x4
s_waitcnt lgkmcnt(0)
v_mov_b32 v1, s26
buffer_store_dword v1, v0, s[4:7], 0 offen offset:512
s_endpgm
