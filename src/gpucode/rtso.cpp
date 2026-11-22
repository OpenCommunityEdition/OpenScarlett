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
        for (auto& y : s.syms) {   // the export's name record precedes the section: payload dword 16 bytes before it
            size_t q = y.name.rfind(".ss_0");
            if (q == std::string::npos || q + 5 != y.name.size() || y.name.rfind("XDXR_", 0) == 0) continue;
            std::string nm = y.name.substr(0, q);
            std::vector<uint8_t> pat(4 + 2 * nm.size(), 0);
            uint32_t len = uint32_t(2 * nm.size());
            memcpy(pat.data(), &len, 4);
            for (size_t k = 0; k < nm.size(); k++) pat[4 + 2 * k] = uint8_t(nm[k]);
            for (size_t o = 16; o + pat.size() <= dx; o++) {   // the first copy of the name (a second, unaligned one follows)
                if (memcmp(b + o, pat.data(), pat.size())) continue;
                s.payload = u(o - 16);
                break;
            }
            break;
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
            if (!traversalSection) {
                uint32_t m = 0;
                for (auto& y : s.syms)
                    if (y.name == "main") m = base + y.offset;
                out.parts.push_back({base, base + (uint32_t)s.size, m, s.payload == 0});
            }
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
    // Hit groups (see rtso.h): a name that is not an export, then three export names or empty strings.
    for (auto& blob : blobs) {
        const uint8_t* b = blob.data();
        size_t n = blob.size();
        auto str = [&](size_t o, std::string& s) -> size_t {   // length-prefixed UTF-16 ASCII at o: bytes used, 0 if not one
            if (o + 4 > n) return 0;
            uint32_t len;
            memcpy(&len, b + o, 4);
            if (len % 2 || len > 512 || o + 4 + len > n) return 0;
            s.clear();
            for (uint32_t k = 0; k < len; k += 2) {
                if (b[o + 4 + k + 1] || b[o + 4 + k] < 0x20 || b[o + 4 + k] > 0x7e) return 0;
                s += char(b[o + 4 + k]);
            }
            return 4 + len;
        };
        for (size_t o = 0; o + 20 <= n; o++) {
            std::string g, sh[3];
            size_t u = str(o, g);
            if (!u || g.empty() || out.exports.count(g) || out.hitgroups.count(g)) continue;
            size_t p = o + u;
            bool ok = true, any = false;
            for (int k = 0; k < 3 && ok; k++) {
                size_t v = str(p, sh[k]);
                ok = v && (sh[k].empty() || out.exports.count(sh[k]));
                any |= ok && !sh[k].empty();
                p += v;
            }
            if (ok && any) out.hitgroups[g] = {sh[0], sh[1], sh[2]};
        }
    }
    if (img.empty()) { if (err) *err = "no sections"; return false; }
    if (!haveMain) { if (err) *err = "no ray-generation scheduler"; return false; }
    while (img.size() % 4) img.push_back(0);
    out.image.assign((const uint32_t*)img.data(), (const uint32_t*)(img.data() + img.size()));
    return true;
}

std::vector<RtLinked> rt_programs(const RtLinked& l) {
    size_t n = 0;
    for (auto& p : l.parts) n += p.raygen && p.main;
    if (n < 2) return {l};
    std::vector<RtLinked> out;
    for (auto& p : l.parts) {
        if (!p.raygen || !p.main) continue;
        RtLinked r = l;
        r.main = p.main;
        r.entries.clear();
        for (auto& q : l.parts)
            if (q.raygen && &q != &p) r.skip.push_back({q.begin, q.end});
        for (uint32_t e : l.entries) {
            bool blank = false;
            for (auto& q : l.parts) blank |= q.raygen && &q != &p && e >= q.begin && e < q.end;
            if (!blank) r.entries.push_back(e);
        }
        out.push_back(std::move(r));
    }
    return out;
}

TranslateOptions rt_translate_options(const RtLinked& l) {
    TranslateOptions o;
    o.code_base = l.base;
    o.entries = l.entries;
    o.global_memory = true;
    o.raw_code = l.image;
    o.raw_skip = l.skip;
    o.raw_rsrc2 = 1 | (4 << 1) | (7 << 7);   // scratch, 4 user SGPRs (runtime table pointer), group ids x/y/z
    o.raw_root_constants = 4;
    o.raw_numthreads = 32;
    o.raw_entry = l.main;
    // Stack slots: the flat group index modulo kRtStackSlots (far more slots than waves in flight).
    o.raw_scratch_offset = "((((gid_.z * RC0_d[0].w + gid_.y) * RC0_d[0].z + gid_.x) % 16384u) * RC0_d[1].x)";
    return o;
}

}  // namespace gpucode
