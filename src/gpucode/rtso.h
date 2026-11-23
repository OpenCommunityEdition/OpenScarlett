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

// Code of collections as one program: segments back to back in `image`, each at its own address
// (TranslateOptions::raw_segments). Offsets below are image offsets unless named addresses.
struct RtLinked {
    uint64_t base = 0x100000000ull;            // code_base: start of the code address space
    std::vector<uint32_t> image;               // dwords
    std::vector<TranslateOptions::RawSegment> segs;
    std::vector<uint32_t> entries;             // every section start and symbol: jump targets
    uint32_t main = 0;                         // the scheduler (of a ray-generation section when there is one)
    bool main_is_raygen = false;
    std::map<std::string, uint64_t> exports;   // export name -> address of <export>.ss_0
    std::vector<uint64_t> traversal;           // address of XDXR_TraceRay.ss_0 of each traversal section, in order
    struct Part { uint32_t begin, end, main; bool raygen; };
    std::vector<Part> parts;                   // non-traversal sections: image range, scheduler, kind
    // Hit groups: name -> closest-hit, any-hit, intersection export names (empty: none). Parsed from
    // records {u32 byte length, UTF-16 name} x 4 (group, closest hit, any hit, intersection) whose
    // shader names are exports of the collection.
    std::map<std::string, std::vector<std::string>> hitgroups;
    // Parking (TranslateOptions::raw_park): set by RtSpace::program.
    bool park = false;
    uint32_t park_ptr_vgpr = 0, resume_at = 0, park_from = 0, park_to = 0;
};

// The code space of a process: console shader identifiers are code addresses, requested on
// collections and mixed freely in shader tables. Every collection's code and every distinct
// traversal section gets a 1 MiB slot chosen by a hash of its contents (from kBase; linear probing
// on collisions), so addresses do not depend on the order titles load collections in. Not
// thread-safe.
class RtSpace {
public:
    static constexpr uint64_t kBase = 0x100000000ull;
    static constexpr uint32_t kSlotShift = 20, kSlots = 1u << 20;   // up to kBase + 1 TiB
    int add(const std::vector<uint8_t>& blob);   // collection id (the same for the same blob), -1: not a collection
    bool identifier(int id, const std::string& name, uint32_t words[3]) const;   // export or hit group of a collection
    // Several collections (and their traversal sections) as one program.
    bool link(const std::vector<int>& ids, RtLinked& out, std::string* err) const;
    // One collection as a program that parks rays leaving it (see TranslateOptions::raw_park).
    bool program(int id, RtLinked& out, std::string* err) const;
    int collection_in_slot(uint32_t slot) const;   // -1: none
    size_t collections() const { return colls_.size(); }
    bool has_raygen(int id) const;
    std::vector<uint64_t> traversal(int id) const;
private:
    struct Block {   // code placed in a slot
        uint64_t address;
        std::vector<uint8_t> bytes;
        std::vector<uint32_t> entries;   // byte offsets in `bytes`
    };
    struct Coll {
        size_t code;                 // block with the collection's own sections
        std::vector<size_t> travs;   // blocks of its traversal sections, in order
        std::vector<uint64_t> traversal;
        std::vector<RtLinked::Part> parts;   // offsets in the code block
        std::map<std::string, uint64_t> exports;
        std::map<std::string, std::vector<std::string>> hitgroups;
    };
    size_t place(std::vector<uint8_t> bytes, std::vector<uint32_t> entries);
    std::vector<Block> blocks_;
    std::map<std::vector<uint8_t>, size_t> block_of_;   // contents -> block (traversal sections shared)
    std::map<uint32_t, size_t> slot_used_;              // slot -> block
    std::map<std::vector<uint8_t>, int> by_blob_;
    std::vector<Coll> colls_;
};
bool rt_link(const std::vector<std::vector<uint8_t>>& blobs, RtLinked& out, std::string* err);

// Root constants of a program: [0..1] runtime table address, [2] groups in x, [3] groups in y,
// [4] stack bytes per wave (scratch wave offset = flat group index * [4]), [6] parking tag
// (serial << 8 | round), [7] parked-lane capacity.
constexpr uint32_t kRtRootConstants = 8;
constexpr uint32_t kRtStackSlots = 16384;
constexpr uint32_t kRtParkVgprs = 64, kRtParkStride = 8 + 4 * kRtParkVgprs;   // parked-lane record (TranslateOptions::raw_park)   // per-wave stack slots (the scratch offset wraps)
TranslateOptions rt_translate_options(const RtLinked& l);
// Shader-cache key of a program (rt_<key>.dxil): the code it includes (offsets and contents),
// scheduler, entries, parking settings and `salt` (translator version and compiler options).
uint64_t rt_cache_key(const RtLinked& l, const std::string& salt);

}  // namespace gpucode
