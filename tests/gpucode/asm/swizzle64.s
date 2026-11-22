// SPDX-License-Identifier: MIT
// ds_swizzle_b32 bit-mask modes in wave64 (each lane holds 1000 + L):
// BROADCAST,4,k reads lane (L & ~3) | k; SWAP,1 reads L ^ 1; SWAP,2 reads L ^ 2;
// SWAP,16 reads L ^ 16 (within each 32-lane group).
;! wave64
;! lanes 1000 + ((L & ~3) | 0)
;! lanes+64 1000 + ((L & ~3) | 2)
;! lanes+128 1000 + ((L & ~3) | 3)
;! lanes+192 1000 + (L ^ 1)
;! lanes+256 1000 + (L ^ 2)
;! lanes+320 1000 + (L ^ 16)
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
v_mbcnt_lo_u32_b32 v3, -1, 0
v_mbcnt_hi_u32_b32 v3, -1, v3
v_add_nc_u32 v3, 1000, v3
ds_swizzle_b32 v1, v3 offset:swizzle(BROADCAST,4,0)
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen
ds_swizzle_b32 v1, v3 offset:swizzle(BROADCAST,4,2)
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen offset:256
ds_swizzle_b32 v1, v3 offset:swizzle(BROADCAST,4,3)
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen offset:512
ds_swizzle_b32 v1, v3 offset:swizzle(SWAP,1)
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen offset:768
ds_swizzle_b32 v1, v3 offset:swizzle(SWAP,2)
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen offset:1024
ds_swizzle_b32 v1, v3 offset:swizzle(SWAP,16)
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v0, s[4:7], 0 offen offset:1280
s_endpgm
