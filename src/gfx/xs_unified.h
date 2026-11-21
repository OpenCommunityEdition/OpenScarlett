// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "xs_object.h"
namespace xs {
struct UnifiedRef { PcObj res; uint64_t offset; uint64_t gpu; };
// If `va` is the CPU address of title GPU-visible memory, return the PC buffer that maps it.
bool unified_lookup(Device* dev, uint64_t va, UnifiedRef* out);
// Translate a console GPU address (possibly a CPU address of unified memory) to a PC GPU address.
uint64_t gpu_va(Device* dev, uint64_t va);
}
