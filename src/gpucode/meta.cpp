// SPDX-License-Identifier: AGPL-3.0-or-later
#include "meta.h"

#include <cstring>

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
}  // namespace

bool parse_root_signature(const uint8_t* d, size_t n, RootSig& rs, std::string* err) {
    rs = RootSig{};
    if (n < 24) return fail(err, "root signature too small");
    rs.version = rd32(d);
    uint32_t np = rd32(d + 4), po = rd32(d + 8), ns = rd32(d + 12), so = rd32(d + 16);
    rs.flags = rd32(d + 20);
    if (po + 12ull * np > n || so + 52ull * ns > n) return fail(err, "bad root signature");
    for (uint32_t i = 0; i < np; i++) {
        uint32_t type = rd32(d + po + 12 * i), vis = rd32(d + po + 12 * i + 4), off = rd32(d + po + 12 * i + 8);
        RsParam p{};
        p.type = ParamType(type);
        p.visibility = vis;
        if (type == 0) {
            if (off + 8ull > n) return fail(err, "bad table");
            uint32_t nr = rd32(d + off), ro = rd32(d + off + 4);
            const unsigned sz = rs.version >= 2 ? 24 : 20;
            uint32_t running = 0;
            for (uint32_t k = 0; k < nr; k++) {
                if (ro + sz * (k + 1ull) > n) return fail(err, "bad range");
                const uint8_t* r = d + ro + sz * k;
                RsRange rr{RangeType(rd32(r)), rd32(r + 4), rd32(r + 8), rd32(r + 12), rd32(r + sz - 4)};
                if (rr.offset == 0xffffffffu) rr.offset = running;
                running = rr.offset + (rr.count == 0xffffffffu ? 0 : rr.count);
                p.ranges.push_back(rr);
            }
        } else if (type == 1) {
            if (off + 12ull > n) return fail(err, "bad constants");
            p.reg = rd32(d + off);
            p.space = rd32(d + off + 4);
            p.num32 = rd32(d + off + 8);
        } else if (type <= 4) {
            if (off + 8ull > n) return fail(err, "bad root descriptor");
            p.reg = rd32(d + off);
            p.space = rd32(d + off + 4);
        } else {
            return fail(err, "unknown root parameter type");
        }
        rs.params.push_back(std::move(p));
    }
    for (uint32_t i = 0; i < ns; i++) {
        RsStaticSampler s{};
        memcpy(s.words, d + so + 52 * i, 52);
        rs.samplers.push_back(s);
    }
    return true;
}

bool parse_meta(const Container& c, Meta& m, std::string* err) {
    m = Meta{};
    if (!c.has_native) return fail(err, "no precompiled code");
    const uint8_t* b = c.precompiled.data();
    const size_t n = c.precompiled.size() / 4;
    auto w = [&](size_t i) { return rd32(b + 4 * i); };
    m.header_dwords = unsigned(c.code_begin / 4);
    if (c.kind == ProgramKind::CS && m.header_dwords == 84) {
        m.rsrc2 = w(78);
        for (int k = 0; k < 3; k++) m.numthreads[k] = w(79 + k);
    }
    if (c.kind == ProgramKind::PS && m.header_dwords == 962) {
        m.ps_input_ena = w(146);
        m.cb_shader_mask = w(148);
        m.db_shader_control = w(147);
        uint32_t ni = w(142);
        for (uint32_t k = 0; k < ni && k < 32; k++) {
            uint32_t a = w(78 + 2 * k), b = w(79 + 2 * k);
            // Bytes: usage, register, dword mask, attribute | default, second register, slot,
            // flags (bit 0 flat, bit 3 16-bit values, bit 4 two of them per dword).
            const uint32_t fl = b >> 24;
            m.ps_inputs.push_back({(a >> 8) & 0xff, (a >> 16) & 0xf, (fl & 1) != 0, a >> 24, (b >> 8) & 0xff, (fl & 8) != 0, (fl & 0x10) != 0});
        }
    }
    if ((c.kind == ProgramKind::VS || c.kind == ProgramKind::MS) && c.precompiled.size() >= 0x2a0 && m.header_dwords * 4u >= 0x2a0) {
        // Output records of 6 bytes at 0x1dc, their count at 0x29c: usage, register, parameter
        // (bit 7: a mesh shader's per-primitive output), register again, slot, flags (bit 0
        // 16-bit values, bit 1 two of them per dword).
        const uint32_t no = rd32(b + 0x29c);
        for (uint32_t k = 0; k < no && k < 32; k++) {
            const uint8_t* e = b + 0x1dc + 6 * k;
            m.vs_outputs.push_back({e[1], uint32_t(e[2] & 0x7f), (e[2] & 0x80) != 0, (e[5] & 1) != 0, (e[5] & 2) != 0});
        }
    }
    // Records follow the s_code_end padding: 5 dwords each, ending with {0, 0, ~0, 0, x}.
    size_t i = c.code_end / 4;
    while (i < n && w(i) == 0xBF9F0000) i++;
    for (; i + 5 <= n; i += 5) {
        uint32_t k = w(i), a = w(i + 1), r2 = w(i + 2), r3 = w(i + 3), code = w(i + 4);
        if (k == 0 && a == 0 && r2 == 0xffffffffu) {
            m.valid = true;
            break;
        }
        if (k >= 0x100 || k == 2) {
            m.resources.push_back({k, a, r2, r3, (code >> 16) & 0xff, (code >> 8) & 0xff, code & 0xff, code >> 24});
        } else {
            m.args.push_back({k, r2, r3, code >> 8, code & 0xff});
        }
    }
    if (!m.valid) return fail(err, "metadata records not found");
    return true;
}

}  // namespace gpucode
