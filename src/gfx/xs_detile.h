// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "xs_object.h"
namespace xs {
// Textures placed on committed title memory: the title's CPU writes there use the console's tiled
// layout and never reach the separate PC resource. Registered textures are converted (detiled with
// the title's own xg_xs.dll) and uploaded before the next submission after a view is made of them.
void detile_register(PcObj pcRes, uint64_t va, const void* consoleDesc, uint32_t initialState);
void detile_view_created(PcObj pcRes);
void detile_flush(PcObj pcDevice, PcObj pcQueue);
void detile_forget(PcObj pcRes);                     // the resource is being destroyed   // before each submission
}
