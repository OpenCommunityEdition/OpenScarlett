// SPDX-License-Identifier: AGPL-3.0-or-later
// Console GPU machine code -> HLSL.
//
// Execution model ("literal"): the HLSL program runs one invocation per console lane and follows
// the console's control flow wave-uniformly. Vector registers are per-invocation values, vector
// writes are predicated on this lane's EXEC bit, scalar registers hold the (identical) scalar
// state in every invocation, and lane masks are kept both as this lane's bit and as the wave's
// integer view (WaveActiveBallot; unused views are removed by the compiler). Cross-lane reads use
// wave and quad intrinsics. The result is exact as long as the PC wave holds the same lanes as the
// console wave. Resources are rebuilt from the binding metadata and the title's root signature, so
// the PC pipeline keeps that root signature.
#pragma once
#include <string>

#include "container.h"
#include "meta.h"

namespace gpucode {

struct TranslateOptions {
    bool comments = true;        // instruction text as comments in the HLSL
    bool stencil_export = false; // SV_StencilRef output (PC GPUs without PSSpecifiedStencilRef reject it)
    // Wave64 compute code on a PC whose waves have 32 lanes: each PC thread runs two console lanes
    // (L and L+32), which keeps wave64 lane masks and cross-lane operations exact. False when the
    // PC runs 64-lane waves.
    bool wave64_dual = true;
    // Dispatcher iterations before an invocation gives up (runaway loops, e.g. walking a linked list
    // with a cycle in bad data, would otherwise reset the PC's GPU). Shaders with barriers use the
    // larger limit: a wave that stops early would leave the others waiting at the barrier.
    unsigned loop_guard = 1u << 16;
    unsigned loop_guard_barrier = 1u << 22;
    bool wave_size_attr = false;
    bool barrier_sync = true;        // false: s_barrier without the group sync (debugging hangs) // the PC runtime takes shader model 6.6 ([WaveSize] on compute)
};

struct TranslateResult {
    bool ok = false;
    std::string hlsl;
    std::string entry = "main";
    std::string profile;         // e.g. "cs_6_0"
    std::string error;           // first unsupported construct, when !ok
    unsigned wave_size = 64;
};

// `rs` is the root signature the shader was compiled against (the container's RTS0 part, or the
// one the title passes when it creates the pipeline).
TranslateResult translate(const Container& c, const RootSig& rs, const TranslateOptions& opt = {});

}  // namespace gpucode
