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
#include <vector>

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
    unsigned loop_guard = 1u << 12;   // dispatcher steps (runaway loops: an invocation stops after this many)
    unsigned loop_guard_barrier = 1u << 22;
    bool wave_size_attr = false;
    // Global memory (title GPU addresses) through the layer's address table: t0 space901 = table,
    // t0.. / u0.. space900 = raw views (docs/gpucode.md). False: only code-embedded data is readable.
    bool global_memory = false;
    // Tests: global memory is the raw buffer at t1 space0 (address = byte offset, high half 0).
    bool global_memory_test = false;
    // Indirect jumps (s_setpc_b64 / s_swappc_b64, e.g. ray-tracing continuation segments): code
    // addresses are code_base + byte offset (s_getpc_b64 returns them); jump targets are block
    // starts, which include every offset in `entries` and every return address after s_swappc.
    uint64_t code_base = 0;
    std::vector<uint32_t> entries;
    // Raw compute programs (code without a container, e.g. linked ray-tracing pipelines): when
    // raw_code is set, the container only supplies its kind; the code, rsrc2 (user SGPRs, group-id
    // enables, scratch), thread count and arguments come from here, and execution starts at the
    // block at raw_entry. raw_scratch_offset is the HLSL for the scratch wave offset SGPR.
    std::vector<uint32_t> raw_code;
    uint32_t raw_rsrc2 = 0, raw_numthreads = 32, raw_entry = 0, raw_root_constants = 0;
    std::string raw_scratch_offset;
    std::vector<std::pair<uint32_t, uint32_t>> raw_skip;   // byte ranges of raw_code left out (not translated)
    // Parking (ray-tracing programs that each hold part of a pipeline's code): an s_swappc_b64 to an
    // address outside the program saves the active lanes' target (address >> 8) and VGPRs
    // v0..raw_park_vgprs-1 to RTST (u0, space930; lane record: target, tag, VGPRs), marks the target
    // in the RTDISC bitmap (u0, space931; bit per 256 bytes from code_base), sets the scheduler's
    // pointer VGPR to 0 (lane done here) and continues at the return address. Root constants [6]
    // = tag (serial << 8 | round), [7] = lane capacity. In rounds > 0, before the instruction at
    // raw_resume_at, lanes parked in the previous round on a target of this program get their VGPRs
    // and pointer back; others get pointer 0; vcc = pointer > 0 (the scheduler's test is redone).
    bool raw_park = false;
    uint32_t raw_park_ptr_vgpr = 0, raw_resume_at = 0, raw_park_vgprs = 64;
    uint32_t raw_park_from = 0, raw_park_to = 0;   // the scheduler's code: only its calls park
    uint32_t raw_park_disc_shift = 0;   // discovery bit = (target - code_base >> 8) >> shift
    // Code at several addresses: raw_code holds segments back to back; a byte offset inside one
    // has address `address + (offset - offset_of_segment)` (empty: code_base + offset).
    struct RawSegment { uint32_t offset, size; uint64_t address; };
    std::vector<RawSegment> raw_segments;
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
