// SPDX-License-Identifier: MIT
// Packed f16 with op_sel/op_sel_hi/neg, and a VOP3 f16 op writing the high half (op_sel dst).
// v1 = (hi 4.0, lo 1.5), v2 = (hi 2.0, lo 0.5), v4 = (hi 0.25, lo -1.0), old destination 0xaaaa5555.
// pk_add: (6, 2) -> 0x46004000; op_sel:[1,0] op_sel_hi:[0,1]: lo 4+0.5, hi 1.5+2 -> 0x43004480;
// pk_fma: lo 1.5*0.5-1 = -0.25, hi 4*2+0.25 = 8.25 -> 0x4820b400;
// pk_mul neg_lo:[1,0]: lo -0.75, hi 8 -> 0x4800ba00; pk_max neg_hi:[0,1]: lo 1.5, hi max(4,-2) -> 0x44003e00;
// (GFX10.1 has no op_sel on VOP3 f16 instructions.)
;! lanes 0x46004000
;! lanes+64 0x43004480
;! lanes+128 0x4820b400
;! lanes+192 0x4800ba00
;! lanes+256 0x44003e00
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
v_mov_b32 v1, 0x44003e00
v_mov_b32 v2, 0x40003800
v_mov_b32 v4, 0x3400bc00
v_pk_add_f16 v3, v1, v2
buffer_store_dword v3, v0, s[4:7], 0 offen
v_pk_add_f16 v3, v1, v2 op_sel:[1,0] op_sel_hi:[0,1]
buffer_store_dword v3, v0, s[4:7], 0 offen offset:256
v_pk_fma_f16 v3, v1, v2, v4
buffer_store_dword v3, v0, s[4:7], 0 offen offset:512
v_pk_mul_f16 v3, v1, v2 neg_lo:[1,0]
buffer_store_dword v3, v0, s[4:7], 0 offen offset:768
v_pk_max_f16 v3, v1, v2 neg_hi:[0,1]
buffer_store_dword v3, v0, s[4:7], 0 offen offset:1024
s_endpgm
