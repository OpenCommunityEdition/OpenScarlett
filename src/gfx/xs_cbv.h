// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "xs_object.h"
namespace xs {
// Record an upload-heap buffer (persistently mapped) so constant data can be found by GPU VA.
void cbv_track_upload(PcObj pcRes, uint64_t va, uint64_t size);
void cbv_untrack(uint64_t va);
// Return a 256-aligned GPU VA holding the constants at `va` (console alignment is 16).
uint64_t cbv_fix_root(Device* dev, uint64_t va);
}
