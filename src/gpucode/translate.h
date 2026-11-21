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
