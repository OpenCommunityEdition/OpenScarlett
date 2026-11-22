// SPDX-License-Identifier: MIT
// image_bvh64_intersect_ray over memory (global-memory test mode: memory = the t1 buffer).
// Box32 node at 0x100 (id 0x25): children 0x40 (z 5..6), 0x48 (z 2..3), none, 0x50 (x 5..6, missed);
// ray (0.5, 0.5, 0) +z: hits sorted by entry distance -> 0x48, 0x40, ~0, ~0.
// Triangle node at 0x200 (id 0x40): (0,0,4) (1,0,4) (0,1,4); ray (0.25, 0.25, 0) +z:
// (t*det, det, u*det, v*det) = (-4, -1, -0.25, -0.25). Triangle node at 0x240 lies off the ray: miss (inf, 1, 0, 0).
;! gmtest
;! mem 0x100 0x40 0x48 0xffffffff 0x50 0.0 0.0 5.0 1.0 1.0 6.0 0.0 0.0 2.0 1.0 1.0 3.0 0.0 0.0 0.0 0.0 0.0 0.0 5.0 0.0 0.0 6.0 1.0 1.0
;! mem 0x200 0.0 0.0 4.0 1.0 0.0 4.0 0.0 1.0 4.0
;! mem 0x240 5.0 5.0 4.0 6.0 5.0 4.0 5.0 6.0 4.0
;! lanes 0x48
;! lanes+64 0x40
;! lanes+128 0xffffffff
;! lanes+192 0xffffffff
;! lanes+256 struct.unpack('<I', struct.pack('<f', -4.0))[0]
;! lanes+320 struct.unpack('<I', struct.pack('<f', -1.0))[0]
;! lanes+384 struct.unpack('<I', struct.pack('<f', -0.25))[0]
;! lanes+448 struct.unpack('<I', struct.pack('<f', -0.25))[0]
;! lanes+512 0x7f800000
;! lanes+576 0x3f800000
s_mov_b32 s6, s5
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[0:3], s[4:5], 0x20
s_lshl_b32 s4, s8, 6
v_add_lshl_u32 v0, v0, s4, 2
s_load_dwordx4 s[4:7], s[6:7], 0x0
s_mov_b32 s20, 0
s_brev_b32 s21, 1
s_mov_b32 s22, -1
s_mov_b32 s23, 0x890003ff
v_mov_b32 v10, 0x25
v_mov_b32 v11, 0
v_mov_b32 v12, 0x42c80000
v_mov_b32 v13, 0.5
v_mov_b32 v14, 0.5
v_mov_b32 v15, 0
v_mov_b32 v16, 0
v_mov_b32 v17, 0
v_mov_b32 v18, 1.0
v_mov_b32 v19, 0x7f800000
v_mov_b32 v20, 0x7f800000
v_mov_b32 v21, 1.0
image_bvh64_intersect_ray v[30:33], v[10:21], s[20:23]
s_waitcnt vmcnt(0)
buffer_store_dword v30, v0, s[4:7], 0 offen
buffer_store_dword v31, v0, s[4:7], 0 offen offset:256
buffer_store_dword v32, v0, s[4:7], 0 offen offset:512
buffer_store_dword v33, v0, s[4:7], 0 offen offset:768
v_mov_b32 v10, 0x40
v_mov_b32 v13, 0x3e800000
v_mov_b32 v14, 0x3e800000
image_bvh64_intersect_ray v[30:33], v[10:21], s[20:23]
s_waitcnt vmcnt(0)
buffer_store_dword v30, v0, s[4:7], 0 offen offset:1024
buffer_store_dword v31, v0, s[4:7], 0 offen offset:1280
buffer_store_dword v32, v0, s[4:7], 0 offen offset:1536
buffer_store_dword v33, v0, s[4:7], 0 offen offset:1792
v_mov_b32 v10, 0x48
image_bvh64_intersect_ray v[30:33], v[10:21], s[20:23]
s_waitcnt vmcnt(0)
buffer_store_dword v30, v0, s[4:7], 0 offen offset:2048
buffer_store_dword v31, v0, s[4:7], 0 offen offset:2304
s_endpgm
