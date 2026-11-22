// SPDX-License-Identifier: MIT
// tbuffer loads/stores on raw buffers use the instruction's format. The input word i is i
// (dispatch_check ramp), so byte k of the input holds (k / 4) >> (8 * (k % 4)) & 255.
// 16_16_UNORM at element L (4 bytes, word L = L): x = L / 65535, y = 0.
// 8_8_8_8_UINT at element L: x = L & 255 (L < 64), y = z = w = 0.
// 16_16_SINT stored from (-2, 7) into out words 192+L: 0x0007fffe.
// 8_UNORM loaded at byte 4L+0 (low byte of word L): x = L / 255.
;! wave64
;! lanes struct.unpack('<I', struct.pack('<f', L / 65535.0))[0]
;! lanes+64 L
;! lanes+128 struct.unpack('<I', struct.pack('<f', L / 255.0))[0]
;! lanes+192 0x0007fffe
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
v_lshlrev_b32 v9, 2, v3
v_mbcnt_lo_u32_b32 v3, -1, 0
v_mbcnt_hi_u32_b32 v3, -1, v3
v_lshlrev_b32 v9, 2, v3
tbuffer_load_format_xy v[10:11], v9, s[0:3], format:23, 0 offen
s_waitcnt vmcnt(0)
buffer_store_dword v10, v0, s[4:7], 0 offen
tbuffer_load_format_xyzw v[10:13], v9, s[0:3], format:60, 0 offen
s_waitcnt vmcnt(0)
buffer_store_dword v10, v0, s[4:7], 0 offen offset:256
tbuffer_load_format_x v10, v9, s[0:3], format:1, 0 offen
s_waitcnt vmcnt(0)
buffer_store_dword v10, v0, s[4:7], 0 offen offset:512
v_mov_b32 v14, -2
v_mov_b32 v15, 7
tbuffer_store_format_xy v[14:15], v0, s[4:7], format:28, 0 offen offset:768
s_endpgm
