// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "xs_object.h"
namespace xs {
// Record an upload-heap buffer (persistently mapped) so constant data can be found by GPU VA.
void cbv_track_upload(PcObj pcRes, uint64_t va, uint64_t size);
void cbv_untrack(uint64_t va);
// Return a 256-aligned GPU VA holding the constants at `va` (console alignment is 16).
uint64_t cbv_fix_root(Device* dev, uint64_t va);
bool cbv_read_gpu(uint64_t gpuVa, void* dst, size_t n);   // diagnostics: bytes of a mapped upload buffer by its GPU address
// Constant-buffer views (descriptor tables) at console alignment: `title_va` as the title passed it,
// `gpu` its PC GPU address. Returns a 256-aligned PC address holding a copy of `size` bytes, or `gpu`
// when the source is not CPU-visible.
uint64_t cbv_fix_view(Device* dev, uint64_t title_va, uint64_t gpu, uint64_t size);
// Re-copies the constant buffer views made since the last submission (called before each submission).
void cbv_refresh();
}
