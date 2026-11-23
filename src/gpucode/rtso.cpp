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
static void parse_hitgroups(const std::vector<uint8_t>& blob, const std::map<std::string, uint64_t>& exports,
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

static uint64_t fnv(const void* p, size_t n, uint64_t h) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

size_t RtSpace::place(std::vector<uint8_t> bytes, std::vector<uint32_t> entries) {
    auto known = block_of_.find(bytes);
    if (known != block_of_.end()) return known->second;
    uint32_t need = uint32_t((bytes.size() + (1u << kSlotShift) - 1) >> kSlotShift);
    if (!need) need = 1;
    uint32_t first = uint32_t(kBase >> kSlotShift);   // slots below kBase are not used
    uint32_t span = kSlots - first - need;
    uint32_t slot = first + uint32_t(fnv(bytes.data(), bytes.size(), 1469598103934665603ull) % span);
    for (;;) {   // linear probing over occupied slots
        bool free = true;
        for (uint32_t k = 0; k < need && free; k++) free = !slot_used_.count(slot + k);
        if (free) break;
        slot = slot + 1 >= first + span ? first : slot + 1;
    }
    size_t b = blocks_.size();
    for (uint32_t k = 0; k < need; k++) slot_used_[slot + k] = b;
    blocks_.push_back({uint64_t(slot) << kSlotShift, bytes, std::move(entries)});
    block_of_[std::move(bytes)] = b;
    return b;
}

int RtSpace::add(const std::vector<uint8_t>& blob) {
    auto known = by_blob_.find(blob);
    if (known != by_blob_.end()) return known->second;
    Coll c;
    std::vector<uint8_t> code;          // the collection's own sections, 256-byte aligned
    std::vector<uint32_t> codeEntries;
    struct Sym { std::string name; uint32_t at; };
    std::vector<Sym> syms;
    for (auto& s : rt_sections(blob.data(), blob.size())) {
        std::vector<uint8_t> bytes(blob.begin() + s.code, blob.begin() + s.code + s.size);
        bool traversalSection = true;
        for (auto& y : s.syms) traversalSection &= y.name == "main" || y.name.rfind("XDXR_TraceRay", 0) == 0;
        if (traversalSection) {
            std::vector<uint32_t> ents{0};
            for (auto& y : s.syms) ents.push_back(y.offset);
            size_t b = place(bytes, ents);
            c.travs.push_back(b);
            for (auto& y : s.syms)
                if (y.name == "XDXR_TraceRay.ss_0") c.traversal.push_back(blocks_[b].address + y.offset);
            continue;
        }
        while (code.size() % 256) code.push_back(0);
        uint32_t base = (uint32_t)code.size();
        code.insert(code.end(), bytes.begin(), bytes.end());
        codeEntries.push_back(base);
        uint32_t m = 0;
        for (auto& y : s.syms) {
            codeEntries.push_back(base + y.offset);
            if (y.name == "main") m = base + y.offset;
            syms.push_back({y.name, base + y.offset});
        }
        c.parts.push_back({base, base + (uint32_t)s.size, m, s.payload == 0});
    }
    if (c.parts.empty()) return -1;
    c.code = place(code, codeEntries);
    for (auto& y : syms) {
        size_t p = y.name.rfind(".ss_0");
        if (y.name.rfind("XDXR_", 0) != 0 && p != std::string::npos && p + 5 == y.name.size())
            c.exports[y.name.substr(0, p)] = blocks_[c.code].address + y.at;
    }
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
        return it == c.exports.end() ? 0u : uint32_t(it->second >> 8);
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

bool RtSpace::has_raygen(int id) const {
    if (id < 0 || id >= (int)colls_.size()) return false;
    for (auto& p : colls_[id].parts)
        if (p.raygen) return true;
    return false;
}
std::vector<uint64_t> RtSpace::traversal(int id) const { return id < 0 || id >= (int)colls_.size() ? std::vector<uint64_t>() : colls_[id].traversal; }
int RtSpace::collection_in_slot(uint32_t slot) const {
    auto it = slot_used_.find(slot);
    if (it == slot_used_.end()) return -1;
    for (size_t c = 0; c < colls_.size(); c++)
        if (colls_[c].code == it->second) return int(c);
    return -1;
}

bool RtSpace::link(const std::vector<int>& ids, RtLinked& out, std::string* err) const {
    out = RtLinked();
    out.base = kBase;
    std::vector<uint8_t> img;
    std::set<size_t> done;
    auto add_block = [&](size_t b) -> uint32_t {   // image offset of the block
        while (img.size() % 256) img.push_back(0);
        uint32_t at = (uint32_t)img.size();
        const Block& k = blocks_[b];
        img.insert(img.end(), k.bytes.begin(), k.bytes.end());
        out.segs.push_back({at, (uint32_t)k.bytes.size(), k.address});
        for (uint32_t e : k.entries) out.entries.push_back(at + e);
        return at;
    };
    for (int id : ids) {
        if (id < 0 || id >= (int)colls_.size()) continue;
        const Coll& c = colls_[id];
        if (out.traversal.empty()) out.traversal = c.traversal;
        for (size_t t : c.travs)
            if (done.insert(t).second) add_block(t);
    }
    for (int id : ids) {
        if (id < 0 || id >= (int)colls_.size()) continue;
        const Coll& c = colls_[id];
        if (!done.insert(c.code).second) continue;
        uint32_t at = add_block(c.code);
        for (auto p : c.parts) out.parts.push_back({at + p.begin, at + p.end, p.main ? at + p.main : 0, p.raygen});
        for (auto& e : c.exports) out.exports.insert(e);
        for (auto& h : c.hitgroups) out.hitgroups.insert(h);
    }
    while (img.size() % 4) img.push_back(0);
    out.image.assign((const uint32_t*)img.data(), (const uint32_t*)(img.data() + img.size()));
    for (auto& p : out.parts)
        if (p.main && (!out.main || (p.raygen && !out.main_is_raygen))) { out.main = p.main; out.main_is_raygen = p.raygen; }
    if (out.parts.empty()) { if (err) *err = "no sections"; return false; }
    if (!out.main) { if (err) *err = "no scheduler"; return false; }
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

bool RtSpace::program(int id, RtLinked& out, std::string* err) const {
    if (!link({id}, out, err)) return false;
    uint32_t ptr, resume;
    if (!park_info(out.image, out.main, &ptr, &resume)) { if (err) *err = "scheduler not understood"; return false; }
    out.park = true;
    out.park_ptr_vgpr = ptr;
    out.resume_at = resume;
    out.park_from = out.main;
    for (auto& p : out.parts)
        if (out.main >= p.begin && out.main < p.end) out.park_to = p.end;
    return true;
}

bool rt_link(const std::vector<std::vector<uint8_t>>& blobs, RtLinked& out, std::string* err) {
    RtSpace s;
    std::vector<int> ids;
    for (auto& b : blobs) ids.push_back(s.add(b));
    return s.link(ids, out, err);
}

uint64_t rt_cache_key(const RtLinked& l, const std::string& salt) {
    uint64_t key = fnv("rt6", 3, 1469598103934665603ull);   // bump when the translation of these programs changes
    key = fnv(salt.data(), salt.size(), key);
    for (auto& sg : l.segs) {
        key = fnv(&sg.address, 8, key);
        key = fnv((const uint8_t*)l.image.data() + sg.offset, sg.size, key);
    }
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
    o.raw_segments = l.segs;
    o.raw_park_disc_shift = RtSpace::kSlotShift - 8;
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
