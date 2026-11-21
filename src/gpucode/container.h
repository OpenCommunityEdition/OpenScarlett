// SPDX-License-Identifier: AGPL-3.0-or-later
// Reader for GDK-compiled shader containers: a DXBC container whose DXIL part carries an XDAT
// directory with the precompiled GPU code (zlib-compressed). Layout notes: docs/provenance.md,
// "Precompiled shader containers".
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gpucode {

constexpr uint32_t fourcc(const char* s) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

enum class ProgramKind : int { PS = 0, VS = 1, GS = 2, HS = 3, DS = 4, CS = 5, Lib = 6, MS = 13, AS = 14, Unknown = -1 };
const char* kind_name(ProgramKind k);

// One element of an input/output signature part (ISG1/OSG1).
struct SigElem {
    std::string name;
    uint32_t index = 0, sysval = 0, comptype = 0, reg = 0, stream = 0, minprec = 0;
    uint8_t mask = 0, rwmask = 0;
    bool is_float() const { return comptype == 3 || comptype == 0; }
    bool is_sint() const { return comptype == 2; }
};
std::vector<SigElem> parse_signature(const std::vector<uint8_t>& part);

// Resource binding from the pipeline-state-validation part (PSV0): type (1 sampler, 2 CBV, 3/4/5
// SRV typed/raw/structured, 6/7/8/9 UAV typed/raw/structured/structured with counter) and kind
// (1 Texture1D ... 9 TextureCubeArray, 10 typed buffer, 11 raw buffer, 12 structured buffer).
struct PsvResource { uint32_t type, space, lo, hi, kind; };
std::vector<PsvResource> parse_psv0(const std::vector<uint8_t>& part);

struct Container {
    std::map<uint32_t, std::vector<uint8_t>> parts;   // fourcc -> part body
    ProgramKind kind = ProgramKind::Unknown;
    int sm_major = 0, sm_minor = 0;
    bool has_bitcode = false;    // false when the bitcode is the 'NOBC' stub (shipping mode "machine code only")
    bool has_native = false;     // a precompiled shader is present
    std::vector<uint8_t> precompiled;   // inflated precompiled blob ('FSSM' structure)
    // Code region inside `precompiled`: the hash-marker instructions, then code up to the first run
    // of s_code_end padding.
    size_t code_begin = 0;       // byte offset of the first instruction (first marker)
    size_t body_begin = 0;       // byte offset after the markers
    size_t code_end = 0;         // byte offset of the padding
    unsigned markers = 0;        // number of leading v_nop+literal marker instructions
    uint32_t marker_literals[4] = {};

    const std::vector<uint8_t>* part(const char* cc) const {
        auto it = parts.find(fourcc(cc));
        return it == parts.end() ? nullptr : &it->second;
    }
    // Machine code after the markers, as dwords.
    std::vector<uint32_t> code() const;
};

// Returns false (with a reason) if `data` is not a DXBC container. A container without precompiled
// code parses successfully with has_native = false.
bool read_container(const void* data, size_t size, Container& out, std::string* err = nullptr);

// Finds the code region of a precompiled blob: [begin, end) in bytes, marker count and literals.
bool locate_code(const uint8_t* blob, size_t size, size_t& begin, size_t& body, size_t& end, unsigned& markers,
                 uint32_t* literals);

}  // namespace gpucode
