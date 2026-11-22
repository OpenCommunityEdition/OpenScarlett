// SPDX-License-Identifier: AGPL-3.0-or-later
// Console ray-tracing state objects: the serialized collections DeserializeStateObjectX takes
// (layout: tools/gpucode/rtso) and their linking into one program for literal translation.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "translate.h"

namespace gpucode {

struct RtSymbol { std::string name; uint32_t offset; };
// payload: the dword 16 bytes before the first copy of the section's export name record (observed: 0 for ray-generation
// shaders, the payload size for hit and miss shaders); -1 when the section has no export.
struct RtSection { size_t code = 0, size = 0; std::vector<RtSymbol> syms; int64_t payload = -1; };
// Sections of a serialized collection (empty if the blob is not one).
std::vector<RtSection> rt_sections(const uint8_t* blob, size_t size);

// Code of all collections of a pipeline in one image: sections 256-byte aligned (identical
// sections once), at `base`. Code addresses (and so shader identifiers) are base + offset.
struct RtLinked {
    uint64_t base = 0x100000000ull;
    std::vector<uint32_t> image;               // dwords
    std::vector<uint32_t> entries;             // every section start and symbol: jump targets
    uint32_t main = 0;                         // the scheduler of the first non-traversal section
    std::map<std::string, uint32_t> exports;   // export name -> offset of <export>.ss_0
    std::vector<uint32_t> traversal;           // XDXR_TraceRay.ss_0 of each traversal section, in order
    struct Part { uint32_t begin, end, main; bool raygen; };
    std::vector<Part> parts;                   // non-traversal sections: image range, scheduler, kind
    std::vector<std::pair<uint32_t, uint32_t>> skip;   // ranges left out of this program (rt_programs)
    // Hit groups: name -> closest-hit, any-hit, intersection export names (empty: none). Parsed from
    // records {u32 byte length, UTF-16 name} x 4 (group, closest hit, any hit, intersection) whose
    // shader names are exports of the pipeline.
    std::map<std::string, std::vector<std::string>> hitgroups;
};
bool rt_link(const std::vector<std::vector<uint8_t>>& blobs, RtLinked& out, std::string* err);

// Root constants of a linked pipeline's program: [0..1] runtime table address, [2] groups in x,
// [3] groups in y, [4] stack bytes per wave (scratch wave offset = flat group index * [4]).
// Programs to compile for a linked pipeline. One program per ray-generation section (the traversal,
// that section and every hit/miss section; other ray-generation code left out), all at
// the full link's addresses so shader identifiers hold in each. A program reached with a raygen
// record that is not its own ends at once (unknown jump target). Few raygens: the full link alone.
std::vector<RtLinked> rt_programs(const RtLinked& l);

constexpr uint32_t kRtRootConstants = 8;
constexpr uint32_t kRtStackSlots = 16384;   // per-wave stack slots (the scratch offset wraps)
TranslateOptions rt_translate_options(const RtLinked& l);

}  // namespace gpucode
