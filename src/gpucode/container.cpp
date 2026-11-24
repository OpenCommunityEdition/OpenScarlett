// SPDX-License-Identifier: AGPL-3.0-or-later
#include "container.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

namespace gpucode {
namespace {
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}
bool inflate_all(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(in);
    z.avail_in = uInt(n);
    out.resize(n * 4 + 4096);
    int r;
    do {
        if (z.total_out == out.size()) out.resize(out.size() * 2);
        z.next_out = out.data() + z.total_out;
        z.avail_out = uInt(out.size() - z.total_out);
        r = inflate(&z, Z_NO_FLUSH);
    } while (r == Z_OK);
    out.resize(z.total_out);
    inflateEnd(&z);
    return r == Z_STREAM_END || (r == Z_BUF_ERROR && z.total_out > 0);
}
// XDAT chunk types (GDK 2026.04 containers).
constexpr uint32_t kXdatBitcode = 0x7, kXdatPrecompiled = 0xd;
}  // namespace

const char* kind_name(ProgramKind k) {
    switch (k) {
    case ProgramKind::PS: return "PS";
    case ProgramKind::VS: return "VS";
    case ProgramKind::GS: return "GS";
    case ProgramKind::HS: return "HS";
    case ProgramKind::DS: return "DS";
    case ProgramKind::CS: return "CS";
    case ProgramKind::Lib: return "lib";
    case ProgramKind::MS: return "MS";
    case ProgramKind::AS: return "AS";
    default: return "?";
    }
}

bool locate_code(const uint8_t* b, size_t size, size_t& begin, size_t& body, size_t& end, unsigned& markers,
                 uint32_t* literals) {
    const size_t n = size / 4;
    auto w = [&](size_t i) { return rd32(b + 4 * i); };
    // Marker: v_nop in VOP3 form (0xD580xxxx) whose first source is a literal.
    auto is_marker = [&](size_t i) { return i + 2 < n && (w(i) >> 16) == 0xD580 && ((w(i + 1) >> 18) & 0x1ff) == 0xff; };
    size_t i = 0;
    while (i < n && !is_marker(i)) i++;
    if (i == n) return false;
    begin = 4 * i;
    markers = 0;
    while (is_marker(i)) {
        if (markers < 4 && literals) literals[markers] = w(i + 2);
        markers++;
        i += 3;
    }
    body = 4 * i;
    size_t e = i;
    while (e + 1 < n && !(w(e) == 0xBF9F0000 && w(e + 1) == 0xBF9F0000)) e++;
    end = e + 1 < n ? 4 * e : 4 * n;
    return true;
}

std::vector<SigElem> parse_signature(const std::vector<uint8_t>& b) {
    std::vector<SigElem> v;
    if (b.size() < 8) return v;
    uint32_t count = rd32(b.data()), off = rd32(b.data() + 4);
    for (uint32_t i = 0; i < count && off + 32ull * (i + 1) <= b.size(); i++) {
        const uint8_t* e = b.data() + off + 32 * i;
        SigElem s;
        s.stream = rd32(e);
        uint32_t name = rd32(e + 4);
        s.index = rd32(e + 8);
        s.sysval = rd32(e + 12);
        s.comptype = rd32(e + 16);
        s.reg = rd32(e + 20);
        s.mask = e[24];
        s.rwmask = e[25];
        s.minprec = rd32(e + 28);
        if (name < b.size()) s.name = std::string(reinterpret_cast<const char*>(b.data() + name), strnlen(reinterpret_cast<const char*>(b.data() + name), b.size() - name));
        v.push_back(s);
    }
    return v;
}

std::vector<PsvResource> parse_psv0(const std::vector<uint8_t>& b) {
    std::vector<PsvResource> v;
    if (b.size() < 4) return v;
    size_t p = 4 + rd32(b.data());
    if (p + 4 > b.size()) return v;
    uint32_t count = rd32(b.data() + p);
    p += 4;
    if (!count || p + 4 > b.size()) return v;
    uint32_t sz = rd32(b.data() + p);
    p += 4;
    if (sz < 16) return v;
    for (uint32_t i = 0; i < count && p + sz * (i + 1ull) <= b.size(); i++) {
        const uint8_t* e = b.data() + p + sz * i;
        v.push_back({rd32(e), rd32(e + 4), rd32(e + 8), rd32(e + 12), sz >= 20 ? rd32(e + 16) : 0});
    }
    return v;
}

// A compute program in the console driver's own container: an 'ACCC' part holding 'FSPX', a small
// directory around the program (middleware ships these beside a root signature, with no DXIL).
//   u32 'FSPX', u32 version, u32 count, u32 total size, count x { u32 kind, u32 offset, u32 size, u32 0 }
//   kind 0: dispatch state, 7 dwords: [3] thread-group size x | y << 16, [4] z (bit 20: wave32),
//           [6] COMPUTE_PGM_RSRC2
//   kind 6: the program, starting with the same hash markers as other precompiled code
//   kind 7: records of 8 dwords; those tagged 0x41 are { 0x41, kind, offset, register, element,
//           parameter << 16 | SGPR << 8 | dwords, ~0, 0 }: the resource records of the other format
//           (0x1xx root descriptors, 0x3xx descriptors of a table, 0x401 static samplers)
// It is rewritten into the layout of that other format (the 84-dword compute header with RSRC2 and
// the thread-group size, the code, the records), so the rest of the translator reads one form.
// Where its arguments arrive is not listed: each root parameter a record names is one.
static bool fspx_to_precompiled(const std::vector<uint8_t>& a, Container& c) {
    if (a.size() < 16 || rd32(a.data()) != fourcc("FSPX")) return false;
    const uint32_t count = rd32(a.data() + 8);
    const size_t total = std::min<size_t>(rd32(a.data() + 12), a.size());
    if (16 + 16ull * count > total) return false;
    const uint8_t *state = nullptr, *code = nullptr, *recs = nullptr;
    size_t stateSize = 0, codeSize = 0, recsSize = 0;
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t* e = a.data() + 16 + 16 * k;
        const uint32_t kind = rd32(e), off = rd32(e + 4), sz = rd32(e + 8);
        if (off < 16 + 16ull * count || off + 1ull * sz > total) continue;
        if (kind == 0) { state = a.data() + off; stateSize = sz; }
        if (kind == 6) { code = a.data() + off; codeSize = sz & ~size_t(3); }
        if (kind == 7) { recs = a.data() + off; recsSize = sz; }
    }
    if (!state || stateSize < 28 || !code || codeSize < 36) return false;
    std::vector<uint32_t> w(84, 0);
    w[78] = rd32(state + 24);
    w[79] = rd32(state + 12) & 0xffff;
    w[80] = rd32(state + 12) >> 16;
    w[81] = rd32(state + 16) & 0xffff;
    // (The entry is padded with s_code_end and a few words after the program: the records must
    // follow the first padding, so it ends there.)
    for (size_t i = 0; i < codeSize; i += 4) {
        if (i + 8 <= codeSize && rd32(code + i) == 0xBF9F0000 && rd32(code + i + 4) == 0xBF9F0000) break;
        w.push_back(rd32(code + i));
    }
    w.push_back(0xBF9F0000);
    w.push_back(0xBF9F0000);
    std::vector<std::pair<uint32_t, uint32_t>> args;   // record kind, parameter << 8 | SGPR
    for (size_t i = 0; i + 32 <= recsSize; i += 4) {
        if (rd32(recs + i) != 0x41 || rd32(recs + i + 24) != 0xffffffffu || rd32(recs + i + 28) != 0) continue;
        const uint32_t kind = rd32(recs + i + 4), offset = rd32(recs + i + 8), reg = rd32(recs + i + 12), elem = rd32(recs + i + 16), where = rd32(recs + i + 20);
        if ((kind >> 8) != 1 && (kind >> 8) != 3 && kind != 0x401) { i += 28; continue; }
        for (uint32_t v : {kind, offset, reg, elem, where}) w.push_back(v);
        if (kind == 0x401) { i += 28; continue; }   // a static sampler: its words are constants in the code
        // Its argument: a descriptor table's pointer (one SGPR) or a root descriptor's address (two).
        const uint32_t key = ((where >> 16) & 0xff) << 8 | ((where >> 8) & 0xff);
        const uint32_t akind = (kind >> 8) == 3 ? 3u : 1u;
        bool seen = false;
        for (auto& x : args) seen |= x.first == akind && x.second == key;
        if (!seen) {
            args.push_back({akind, key});
            for (uint32_t v : {akind, akind == 3 ? 1u : 2u, akind == 3 ? 0u : reg, 0u, key}) w.push_back(v);
        }
        i += 28;
    }
    for (uint32_t v : {0u, 0u, 0xffffffffu, 0u, 0u}) w.push_back(v);
    c.precompiled.assign((const uint8_t*)w.data(), (const uint8_t*)w.data() + w.size() * 4);
    c.kind = ProgramKind::CS;
    c.wave_hint = (rd32(state + 16) >> 20) & 1 ? 32 : 64;
    c.has_bitcode = false;
    return locate_code(c.precompiled.data(), c.precompiled.size(), c.code_begin, c.body_begin, c.code_end, c.markers, c.marker_literals);
}

std::vector<uint32_t> Container::code() const {
    std::vector<uint32_t> v((code_end - body_begin) / 4);
    if (!v.empty()) memcpy(v.data(), precompiled.data() + body_begin, v.size() * 4);
    return v;
}

bool read_container(const void* data, size_t size, Container& c, std::string* err) {
    auto* d = static_cast<const uint8_t*>(data);
    c = Container{};
    if (size < 32 || rd32(d) != fourcc("DXBC")) return fail(err, "not a DXBC container");
    uint32_t total = rd32(d + 24), count = rd32(d + 28);
    if (total > size || 32 + 4ull * count > total) return fail(err, "bad container header");
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd32(d + 32 + 4 * i);
        if (off + 8ull > total) return fail(err, "bad part offset");
        uint32_t cc = rd32(d + off), sz = rd32(d + off + 4);
        if (off + 8ull + sz > total) return fail(err, "bad part size");
        c.parts[cc].assign(d + off + 8, d + off + 8 + sz);
    }
    auto it = c.parts.find(fourcc("DXIL"));
    if (it == c.parts.end() || it->second.size() < 24) {
        if (auto accc = c.parts.find(fourcc("ACCC")); accc != c.parts.end()) c.has_native = fspx_to_precompiled(accc->second, c);
        return true;
    }
    const auto& p = it->second;
    // Program header: version (kind << 16 | major << 4 | minor), size in dwords, 'DXIL', DXIL version,
    // bitcode offset (from the 'DXIL' magic), bitcode size.
    uint32_t ver = rd32(p.data());
    c.kind = ProgramKind(int(ver >> 16));
    c.sm_major = int((ver >> 4) & 0xf);
    c.sm_minor = int(ver & 0xf);
    uint32_t bcOff = rd32(p.data() + 16), bcSize = rd32(p.data() + 20);
    const size_t x = 24;   // XDAT follows the program header when present
    if (bcOff == 16 || p.size() < x + 12 || rd32(p.data() + x) != fourcc("XDAT")) {
        c.has_bitcode = bcSize >= 4 && 8ull + bcOff + 4 <= p.size() && rd32(p.data() + 8 + bcOff) != fourcc("NOBC");
        return true;
    }
    uint32_t entries = rd32(p.data() + x + 4);
    if (x + 12 + 12ull * entries > p.size()) return fail(err, "bad XDAT directory");
    for (uint32_t k = 0; k < entries; k++) {
        const uint8_t* e = p.data() + x + 12 + 12 * k;
        uint32_t type = rd32(e), off = rd32(e + 4), sz = rd32(e + 8);
        // Offsets are relative to the XDAT header. Recorded sizes of the last entries can run up to
        // 24 bytes past the part's end (GDK 2026.04); clamp to the bytes present.
        if (x + off > p.size()) continue;
        if (x + off + 1ull * sz > p.size()) sz = uint32_t(p.size() - x - off);
        const uint8_t* body = p.data() + x + off;
        if (type == kXdatBitcode) c.has_bitcode = sz >= 4 && rd32(body) != fourcc("NOBC");
        if (type == kXdatPrecompiled) {
            if (!inflate_all(body, sz, c.precompiled)) return fail(err, "precompiled shader does not inflate");
            c.has_native = locate_code(c.precompiled.data(), c.precompiled.size(), c.code_begin, c.body_begin,
                                       c.code_end, c.markers, c.marker_literals);
        }
    }
    return true;
}

}  // namespace gpucode
