// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "xs_object.h"
namespace xs {
struct UnifiedRef { PcObj res; uint64_t offset; uint64_t gpu; };
// If `va` is the CPU address of title GPU-visible memory, return the PC buffer that maps it.
bool unified_lookup(Device* dev, uint64_t va, UnifiedRef* out);
// The same for an address where a physical page (section memory) is mapped; run: bytes from there
// on that follow each other in the section.
bool section_lookup(Device* dev, uint64_t va, UnifiedRef* out, uint64_t* run);
bool unified_block(uint64_t va, uint64_t* base, uint64_t* size);   // the title's graphics allocation an address lies in
bool unified_from_gpu(uint64_t gpuVa, UnifiedRef* out);   // the buffer a PC GPU address of unified memory lies in
// Translate a console GPU address (possibly a CPU address of unified memory) to a PC GPU address.
uint64_t gpu_va(Device* dev, uint64_t va);
bool unified_read_gpu(uint64_t gpuVa, void* dst, size_t n);   // diagnostics: bytes of zero-copy memory at a PC GPU address
uint64_t unified_cpu_of_gpu(uint64_t gpuVa);   // the CPU address of unified memory at a PC GPU address (0: none)
}
