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
struct RtSection { size_t code = 0, size = 0; std::vector<RtSymbol> syms; };
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
};
bool rt_link(const std::vector<std::vector<uint8_t>>& blobs, RtLinked& out, std::string* err);

// Root constants of a linked pipeline's program: [0..1] runtime table address, [2] groups in x,
// [3] groups in y, [4] stack bytes per wave (scratch wave offset = flat group index * [4]).
constexpr uint32_t kRtRootConstants = 8;
constexpr uint32_t kRtStackSlots = 16384;   // per-wave stack slots (the scratch offset wraps)
TranslateOptions rt_translate_options(const RtLinked& l);

}  // namespace gpucode
