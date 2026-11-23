// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rtso.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "isa.h"

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

// Hit groups of one collection (see rtso.h): a name that is not an export, then three export names
// or empty strings.
static void parse_hitgroups(const std::vector<uint8_t>& blob, const std::map<std::string, uint32_t>& exports,
                            std::map<std::string, std::vector<std::string>>& out) {
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
        if (!u || g.empty() || exports.count(g) || out.count(g)) continue;
        size_t p = o + u;
        bool ok = true, any = false;
        for (int k = 0; k < 3 && ok; k++) {
            size_t v = str(p, sh[k]);
            ok = v && (sh[k].empty() || exports.count(sh[k]));
            any |= ok && !sh[k].empty();
            p += v;
        }
        if (ok && any) out[g] = {sh[0], sh[1], sh[2]};
    }
}

int RtSpace::add(const std::vector<uint8_t>& blob) {
    auto known = by_blob_.find(blob);
    if (known != by_blob_.end()) return known->second;
    Coll c;
    for (auto& s : rt_sections(blob.data(), blob.size())) {
        std::vector<uint8_t> code(blob.begin() + s.code, blob.begin() + s.code + s.size);
        bool traversalSection = true;
        for (auto& y : s.syms) traversalSection &= y.name == "main" || y.name.rfind("XDXR_TraceRay", 0) == 0;
        uint32_t base;
        auto seen = by_code_.find(code);
        bool fresh = seen == by_code_.end();
        if (fresh) {   // identical sections (the traversal in every collection) are placed once
            while (img_.size() % 256) img_.push_back(0);
            base = (uint32_t)img_.size();
            img_.insert(img_.end(), code.begin(), code.end());
            by_code_[code] = base;
        } else base = seen->second;
        std::vector<uint32_t> ents{base};
        for (auto& y : s.syms) ents.push_back(base + y.offset);
        if (traversalSection) {
            for (auto& y : s.syms)
                if (y.name == "XDXR_TraceRay.ss_0") c.traversal.push_back(base + y.offset);
            size_t t = travs_.size();
            for (size_t k = 0; k < travs_.size(); k++)
                if (travs_[k].begin == base) t = k;
            if (t == travs_.size()) travs_.push_back({base, base + (uint32_t)s.size, ents});
            c.travs.push_back(t);
            continue;
        }
        uint32_t m = 0;
        for (auto& y : s.syms)
            if (y.name == "main") m = base + y.offset;
        size_t part = parts_.size();
        for (size_t k = 0; k < parts_.size(); k++)
            if (parts_[k].begin == base) part = k;
        if (part == parts_.size()) {
            parts_.push_back({base, base + (uint32_t)s.size, m, s.payload == 0});
            part_entries_.push_back(ents);
        }
        c.parts.push_back(part);
        for (auto& y : s.syms) {
            size_t p = y.name.rfind(".ss_0");
            if (y.name.rfind("XDXR_", 0) != 0 && p != std::string::npos && p + 5 == y.name.size()) c.exports[y.name.substr(0, p)] = base + y.offset;
        }
    }
    if (c.parts.empty() && c.traversal.empty()) return -1;
    parse_hitgroups(blob, c.exports, c.hitgroups);
    colls_.push_back(std::move(c));
    by_blob_[blob] = int(colls_.size() - 1);
    return int(colls_.size() - 1);
}

bool RtSpace::identifier(int id, const std::string& name, uint32_t words[3]) const {
    if (id < 0 || id >= (int)colls_.size()) return false;
    const Coll& c = colls_[id];
    auto ptr = [&](const std::string& x) -> uint32_t {
        auto it = c.exports.find(x);
        return it == c.exports.end() ? 0u : uint32_t((kBase + it->second) >> 8);
    };
    words[0] = words[1] = words[2] = 0;
    auto hg = c.hitgroups.find(name);
    if (hg != c.hitgroups.end()) {   // closest hit, any hit, intersection (0: none)
        for (int k = 0; k < 3; k++) words[k] = hg->second[k].empty() ? 0u : ptr(hg->second[k]);
        return true;
    }
    words[0] = ptr(name);
    return words[0] != 0;
}

bool RtSpace::link(const std::vector<int>& ids, RtLinked& out, std::string* err) const {
    out = RtLinked();
    out.base = kBase;
    std::vector<uint8_t> img = img_;
    while (img.size() % 4) img.push_back(0);
    out.image.assign((const uint32_t*)img.data(), (const uint32_t*)(img.data() + img.size()));
    std::vector<bool> in(parts_.size(), false), tin(travs_.size(), false);
    for (int id : ids) {
        if (id < 0 || id >= (int)colls_.size()) continue;
        const Coll& c = colls_[id];
        if (out.traversal.empty()) out.traversal = c.traversal;
        for (size_t t : c.travs) tin[t] = true;
        for (size_t p : c.parts) in[p] = true;
        for (auto& e : c.exports) out.exports.insert(e);
        for (auto& h : c.hitgroups) out.hitgroups.insert(h);
    }
    for (size_t t = 0; t < travs_.size(); t++) {   // only the collections' own traversal sections
        if (!tin[t]) { out.skip.push_back({travs_[t].begin, travs_[t].end}); continue; }
        out.entries.insert(out.entries.end(), travs_[t].entries.begin(), travs_[t].entries.end());
    }
    for (size_t p = 0; p < parts_.size(); p++) {
        if (!in[p]) { out.skip.push_back({parts_[p].begin, parts_[p].end}); continue; }
        out.parts.push_back(parts_[p]);
        out.entries.insert(out.entries.end(), part_entries_[p].begin(), part_entries_[p].end());
    }
    for (auto& p : out.parts)
        if (p.main && (!out.main || (p.raygen && !out.main_is_raygen))) { out.main = p.main; out.main_is_raygen = p.raygen; }
    // The image ends with the last included section (what follows depends on later collections).
    uint32_t end = 0;
    for (auto& p : out.parts) end = std::max(end, p.end);
    for (size_t t = 0; t < travs_.size(); t++)
        if (tin[t]) end = std::max(end, travs_[t].end);
    end = (end + 3) & ~3u;
    if (end < out.image.size() * 4) out.image.resize(end / 4);
    std::vector<std::pair<uint32_t, uint32_t>> sk;
    for (auto& r : out.skip)
        if (r.first < end) sk.push_back({r.first, std::min(r.second, end)});
    out.skip.swap(sk);
    if (out.parts.empty()) { if (err) *err = "no sections"; return false; }
    if (!out.main) { if (err) *err = "no ray-generation scheduler"; return false; }
    return true;
}

// The scheduler's resume point for parked rays: the pointer VGPR is the one v_readfirstlane_b32
// reads before the first s_swappc_b64; the resume point is the s_and_saveexec_b32 after the first
// "v_cmp_lt_i32 vcc, 0, <pointer>" (lanes with a pointer > 0 enter the loop there).
static bool park_info(const std::vector<uint32_t>& img, uint32_t main, uint32_t* ptr, uint32_t* resume) {
    std::vector<Instr> v;
    for (size_t i = main / 4; i < img.size() && v.size() < 600;) {
        Instr in;
        if (!decode(img.data(), img.size(), i, in) || !in.len) break;
        v.push_back(in);
        i += in.len;
        if (in.name && !strcmp(in.name, "s_endpgm")) break;
    }
    int p = -1;
    for (size_t k = 0; k < v.size() && p < 0; k++) {
        if (!v[k].name || strcmp(v[k].name, "s_swappc_b64")) continue;
        for (size_t j = k; j-- > 0 && p < 0;)
            if (v[j].name && !strcmp(v[j].name, "v_readfirstlane_b32") && v[j].src0 >= 256) p = v[j].src0 - 256;
    }
    if (p < 0) return false;
    for (size_t k = 0; k < v.size(); k++) {
        auto& in = v[k];
        if (!in.name || strcmp(in.name, "v_cmp_lt_i32") || in.fmt != Fmt::VOPC || in.src0 != 128 || in.src1 != 256 + p) continue;
        for (size_t j = k + 1; j < v.size() && j < k + 12; j++)
            if (v[j].name && !strcmp(v[j].name, "s_and_saveexec_b32")) {
                *ptr = uint32_t(p);
                *resume = v[j].offset;
                return true;
            }
    }
    return false;
}

std::vector<int> RtSpace::parts_of(int id) const {
    std::vector<int> r;
    if (id >= 0 && id < (int)colls_.size())
        for (size_t p : colls_[id].parts) r.push_back(int(p));
    return r;
}

int RtSpace::collection_at(uint32_t offset) const {
    for (size_t c = 0; c < colls_.size(); c++)
        for (size_t p : colls_[c].parts)
            if (offset >= parts_[p].begin && offset < parts_[p].end) return int(c);
    return -1;
}

bool RtSpace::program(int id, RtLinked& out, std::string* err) const {
    if (!link({id}, out, err)) return false;
    // A collection without a scheduler of its own (none observed) borrows any.
    if (!out.main)
        for (auto& p : parts_)
            if (p.main) { out.main = p.main; break; }
    uint32_t ptr, resume;
    if (!park_info(out.image, out.main, &ptr, &resume)) { if (err) *err = "scheduler not understood"; return false; }
    out.park = true;
    out.park_ptr_vgpr = ptr;
    out.resume_at = resume;
    out.park_from = out.main;
    for (auto& p : parts_)
        if (out.main >= p.begin && out.main < p.end) out.park_to = p.end;
    return true;
}

bool rt_link(const std::vector<std::vector<uint8_t>>& blobs, RtLinked& out, std::string* err) {
    RtSpace s;
    std::vector<int> ids;
    for (auto& b : blobs) ids.push_back(s.add(b));
    return s.link(ids, out, err);
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

static uint64_t fnv(const void* p, size_t n, uint64_t h) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint64_t rt_cache_key(const RtLinked& l, const std::string& salt) {
    uint64_t key = fnv("rt3", 3, 1469598103934665603ull);
    key = fnv(salt.data(), salt.size(), key);
    std::vector<std::pair<uint32_t, uint32_t>> sk = l.skip;
    std::sort(sk.begin(), sk.end());
    uint32_t at = 0, end = (uint32_t)l.image.size() * 4;
    for (auto& [b, e] : sk) {
        if (b > at) { key = fnv(&at, 4, key); key = fnv((const uint8_t*)l.image.data() + at, std::min(b, end) - at, key); }
        at = std::max(at, e);
    }
    if (at < end) { key = fnv(&at, 4, key); key = fnv((const uint8_t*)l.image.data() + at, end - at, key); }
    uint32_t v[6] = {l.main, l.park ? 1u : 0u, l.park_ptr_vgpr, l.resume_at, l.park_from, l.park_to};
    key = fnv(v, sizeof v, key);
    return fnv(l.entries.data(), l.entries.size() * 4, key);
}

TranslateOptions rt_translate_options(const RtLinked& l) {
    TranslateOptions o;
    o.code_base = l.base;
    o.entries = l.entries;
    o.global_memory = true;
    o.raw_code = l.image;
    o.raw_skip = l.skip;
    o.raw_park = l.park;
    o.raw_park_ptr_vgpr = l.park_ptr_vgpr;
    o.raw_resume_at = l.resume_at;
    o.raw_park_from = l.park_from;
    o.raw_park_to = l.park_to;
    o.raw_park_vgprs = kRtParkVgprs;
    o.raw_rsrc2 = 1 | (4 << 1) | (7 << 7);   // scratch, 4 user SGPRs (runtime table pointer), group ids x/y/z
    o.raw_root_constants = 4;
    o.raw_numthreads = 32;
    o.raw_entry = l.main;
    // Stack slots: the flat group index modulo kRtStackSlots (far more slots than waves in flight).
    o.raw_scratch_offset = "((((gid_.z * RC0_d[0].w + gid_.y) * RC0_d[0].z + gid_.x) % 16384u) * RC0_d[1].x)";
    return o;
}

}  // namespace gpucode
