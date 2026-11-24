// SPDX-License-Identifier: AGPL-3.0-or-later
// Root signatures (RTS0, the standard Direct3D 12 serialized form) and the binding metadata the
// console compiler stores next to precompiled code. Layout notes: docs/provenance.md,
// "Precompiled shader metadata".
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "container.h"

namespace gpucode {

enum class RangeType : uint8_t { SRV = 0, UAV = 1, CBV = 2, Sampler = 3 };
enum class ParamType : uint8_t { Table = 0, Constants = 1, CBV = 2, SRV = 3, UAV = 4 };

struct RsRange {
    RangeType type;
    uint32_t count, base, space, offset;   // offset in descriptors from the table start (appends resolved)
};
struct RsParam {
    ParamType type;
    uint32_t visibility = 0;
    uint32_t reg = 0, space = 0, num32 = 0;   // root descriptors / constants
    std::vector<RsRange> ranges;               // tables
};
struct RsStaticSampler {
    uint32_t words[13];   // D3D12_STATIC_SAMPLER_DESC as serialized
    uint32_t reg() const { return words[10]; }
    uint32_t space() const { return words[11]; }
};
struct RootSig {
    uint32_t version = 0, flags = 0;
    std::vector<RsParam> params;
    std::vector<RsStaticSampler> samplers;
};
bool parse_root_signature(const uint8_t* data, size_t size, RootSig& out, std::string* err = nullptr);

// One argument the shader receives in a user SGPR (or, with the memory-based ABI, in memory).
struct UserArg {
    uint32_t kind;     // 1 root descriptor, 3 descriptor table, 0 root constants, 5/6 heap base pointers, 4 other
    uint32_t size;     // SGPRs
    uint32_t reg;      // shader register of a root descriptor/constants
    uint32_t param;    // root parameter index
    uint32_t loc;      // < 32: first SGPR; >= 32: dword (loc - 32) of the memory-based argument block
};
// One resource the shader accesses.
struct ResRecord {
    uint32_t kind;     // high byte: 1 root descriptor, 3 table entry, 4 static sampler, 0 root constants;
                       // low byte: 0 SRV, 1 sampler, 2 CBV, 3 UAV
    uint32_t offset;   // dwords from the table start (table entries)
    uint32_t reg;      // shader register
    uint32_t elem;     // structured stride in bytes; 1 for raw buffers; 4 for typed resources
    uint32_t param, loc, dwords;   // root parameter, SGPR/memory location of its argument, descriptor size
    uint32_t count = 0;            // array size for resource arrays (0: single)
    uint32_t cls() const { return kind & 0xff; }
    uint32_t where() const { return kind >> 8; }
};

struct Meta {
    bool valid = false;
    unsigned header_dwords = 0;
    // Compute: COMPUTE_PGM_RSRC2 fields (AMD's public register layout) and the thread-group size.
    uint32_t rsrc2 = 0;
    uint32_t numthreads[3] = {1, 1, 1};
    // Pixel shaders (962-dword header): SPI_PS_INPUT_ENA (AMD's public register layout), the
    // interpolated inputs ({signature register, component mask, flat}), colour-target mask and
    // depth-export control.
    uint32_t ps_input_ena = 0, cb_shader_mask = 0, db_shader_control = 0;
    // reg: the input signature register; mask: the parameter's dwords in use; attr: the attribute
    // v_interp names. fp16: 16-bit values, one per dword (low half) unless pack2: then two per
    // dword (element 2d in the low half of dword d, 2d + 1 in the high half), this register's
    // elements first and, from the next whole dword, those of register row2 (0: none).
    struct PsInput { uint32_t reg, mask; bool flat; uint32_t attr = 0, row2 = 0; bool fp16 = false, pack2 = false; };
    std::vector<PsInput> ps_inputs;
    // Vertex and mesh shaders: what `exp param<param>` writes. row: the output signature register
    // (the primitive signature's when prim, mesh shaders only); fp16 / pack2 as for pixel inputs,
    // the second register of a packed pair being the next 16-bit register no record names.
    struct VsOutput { uint32_t row, param; bool prim, fp16, pack2; };
    std::vector<VsOutput> vs_outputs;
    std::vector<UserArg> args;
    std::vector<ResRecord> resources;
    unsigned user_sgprs() const { return (rsrc2 >> 1) & 31; }
    bool tgid_en(int i) const { return (rsrc2 >> (7 + i)) & 1; }
    bool tg_size_en() const { return (rsrc2 >> 10) & 1; }
    unsigned tid_comps() const { return ((rsrc2 >> 11) & 3) + 1; }
    unsigned lds_bytes() const { return ((rsrc2 >> 15) & 0x1ff) * 512; }
};
bool parse_meta(const Container& c, Meta& out, std::string* err = nullptr);

}  // namespace gpucode
