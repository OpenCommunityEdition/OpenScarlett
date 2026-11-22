// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rtso.h"

#include <cstring>
#include <set>

namespace gpucode {

std::vector<RtSection> rt_sections(const uint8_t* b, size_t n) {
    std::vector<RtSection> out;
    auto u = [&](size_t o) { uint32_t v = 0; if (o + 4 <= n) memcpy(&v, b + o, 4); return v; };
    for (size_t dx = 4; dx + 28 <= n; dx++) {
        if (memcmp(b + dx, "DXBC", 4)) continue;
        uint32_t sz = u(dx - 4);
        if (u(dx + 24) != sz) continue;   // DXBC total size matches the size prefix
        size_t h = dx + sz;
        if (u(h) != 0x1c) break;          // 15-dword code header
        RtSection s;
        s.code = h + 60;
        s.size = u(h + 56);
        if (s.code + s.size > n) break;
        size_t tr = s.code + s.size + 0x3c, recs = tr + 8;
        uint32_t cnt = u(tr);
        for (uint32_t i = 0; i < cnt && recs + i * 24 + 24 <= n; i++) {
            size_t r = recs + i * 24, no = recs + u(r);
            std::string name;
            while (no < n && b[no]) name += char(b[no++]);
            s.syms.push_back({name, u(r + 8)});
        }
        out.push_back(s);
        dx = s.code + s.size;
    }
    return out;
}

bool rt_link(const std::vector<std::vector<uint8_t>>& blobs, RtLinked& out, std::string* err) {
    std::vector<uint8_t> img;
    std::set<std::vector<uint8_t>> seen;
    bool haveMain = false;
    for (auto& blob : blobs) {
        for (auto& s : rt_sections(blob.data(), blob.size())) {
            std::vector<uint8_t> code(blob.begin() + s.code, blob.begin() + s.code + s.size);
            if (!seen.insert(code).second) continue;   // the traversal sections repeat in every collection
            while (img.size() % 256) img.push_back(0);
            uint32_t base = (uint32_t)img.size();
            img.insert(img.end(), code.begin(), code.end());
            out.entries.push_back(base);
            bool traversalSection = true;
            for (auto& y : s.syms) traversalSection &= y.name == "main" || y.name.rfind("XDXR_TraceRay", 0) == 0;
            for (auto& y : s.syms) {
                bool isImportStub = y.name.rfind("XDXR_TraceRay", 0) == 0 && !traversalSection;
                out.entries.push_back(base + y.offset);
                if (y.name == "XDXR_TraceRay.ss_0" && traversalSection) out.traversal.push_back(base + y.offset);
                if (y.name == "main" && !traversalSection && !haveMain) { out.main = base + y.offset; haveMain = true; }
                size_t p = y.name.rfind(".ss_0");
                if (!isImportStub && p != std::string::npos && p + 5 == y.name.size() && y.name.rfind("XDXR_", 0) != 0)
                    out.exports[y.name.substr(0, p)] = base + y.offset;
            }
        }
    }
    if (img.empty()) { if (err) *err = "no sections"; return false; }
    if (!haveMain) { if (err) *err = "no ray-generation scheduler"; return false; }
    while (img.size() % 4) img.push_back(0);
    out.image.assign((const uint32_t*)img.data(), (const uint32_t*)(img.data() + img.size()));
    return true;
}

TranslateOptions rt_translate_options(const RtLinked& l) {
    TranslateOptions o;
    o.code_base = l.base;
    o.entries = l.entries;
    o.global_memory = true;
    o.raw_code = l.image;
    o.raw_rsrc2 = 1 | (4 << 1) | (7 << 7);   // scratch, 4 user SGPRs (runtime table pointer), group ids x/y/z
    o.raw_root_constants = 4;
    o.raw_numthreads = 32;
    o.raw_entry = l.main;
    // Stack slots: the flat group index modulo kRtStackSlots (far more slots than waves in flight).
    o.raw_scratch_offset = "((((gid_.z * RC0_d[0].w + gid_.y) * RC0_d[0].z + gid_.x) % 16384u) * RC0_d[1].x)";
    return o;
}

}  // namespace gpucode
