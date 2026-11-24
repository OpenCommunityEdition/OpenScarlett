// SPDX-License-Identifier: AGPL-3.0-or-later
// xsl-gpucode: command-line front end of the console GPU code translator.
//   xsl-gpucode dis <code.bin>          disassemble a raw code blob
//   xsl-gpucode dis-elf <file.elf>      list every instruction of every symbol in an amdgcn ELF:
//                                       "<symbol>\t<offset>\t<len>\t<mnemonic>" (decoder tests)
//   xsl-gpucode cso <file.cso>          container summary and disassembly of its precompiled code
//   xsl-gpucode cso2elf <out.elf> <file.cso>...   wrap the containers' code in an amdgcn ELF
//   xsl-gpucode translate <file.cso> [<rootsig.bin>]   HLSL for the container's code (root signature:
//                                       the container's RTS0 part unless given)
//   xsl-gpucode meta <file.cso>         binding metadata
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "container.h"
#include "isa.h"
#include "meta.h"
#include "rtso.h"
#include "translate.h"

using namespace gpucode;

static std::vector<uint8_t> read_file(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static int dis(const std::vector<uint8_t>& d, size_t off, size_t size, const char* sym, bool tsv) {
    std::vector<uint32_t> w(size / 4);
    memcpy(w.data(), d.data() + off, w.size() * 4);
    for (auto& in : decode_all(w.data(), w.size())) {
        if (tsv)
            printf("%s\t%u\t%u\t%s\n", sym, in.offset, in.len, mnemonic_with_encoding(in).c_str());
        else
            printf("%6x: %s\n", in.offset, disasm(in).c_str());
    }
    return 0;
}

static int dis_elf(const std::vector<uint8_t>& d) {
    auto u16 = [&](size_t o) { uint16_t v; memcpy(&v, d.data() + o, 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, d.data() + o, 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v; memcpy(&v, d.data() + o, 8); return v; };
    if (d.size() < 64 || memcmp(d.data(), "\x7f" "ELF\x02", 5)) return fprintf(stderr, "not ELF64\n"), 1;
    uint64_t shoff = u64(0x28);
    unsigned shnum = u16(0x3c);
    struct Sec { uint32_t type; uint64_t off, size; uint32_t link; };
    std::vector<Sec> s(shnum);
    for (unsigned i = 0; i < shnum; i++) {
        size_t b = shoff + 64 * i;
        s[i] = {u32(b + 4), u64(b + 24), u64(b + 32), u32(b + 40)};
    }
    for (auto& sec : s) {
        if (sec.type != 2) continue;   // SHT_SYMTAB
        const Sec& str = s[sec.link];
        for (uint64_t o = sec.off + 24; o + 24 <= sec.off + sec.size; o += 24) {
            uint32_t name = u32(o);
            uint16_t shndx = u16(o + 6);
            uint64_t val = u64(o + 8), size = u64(o + 16);
            if (!size || shndx >= shnum) continue;
            const char* nm = (const char*)d.data() + str.off + name;
            dis(d, s[shndx].off + val, size, nm, true);
        }
    }
    return 0;
}

static bool load_cso(const char* path, Container& c) {
    auto d = read_file(path);
    std::string err;
    if (!read_container(d.data(), d.size(), c, &err)) {
        fprintf(stderr, "%s: %s\n", path, err.c_str());
        return false;
    }
    return true;
}

static int cso(const char* path) {
    Container c;
    if (!load_cso(path, c)) return 1;
    printf("%s: %s %d.%d, bitcode %s, precompiled %s", path, kind_name(c.kind), c.sm_major, c.sm_minor,
           c.has_bitcode ? "yes" : "no", c.has_native ? "yes" : "no");
    printf(", parts:");
    for (auto& [cc, body] : c.parts) printf(" %.4s(%zu)", (const char*)&cc, body.size());
    printf("\n");
    if (!c.has_native) return 0;
    printf("precompiled %zu bytes, code at 0x%zx..0x%zx, %u markers:", c.precompiled.size(), c.body_begin, c.code_end, c.markers);
    for (unsigned k = 0; k < c.markers && k < 4; k++) printf(" %08x", c.marker_literals[k]);
    printf("\n");
    auto w = c.code();
    for (auto& in : decode_all(w.data(), w.size())) printf("%6x: %s\n", in.offset, disasm(in).c_str());
    return 0;
}

static int cso2elf(const char* out, int n, char** paths) {
    std::vector<uint8_t> text, strtab{0}, symtab(24, 0);
    for (int i = 0; i < n; i++) {
        Container c;
        if (!load_cso(paths[i], c) || !c.has_native) continue;
        auto w = c.code();
        std::string name = paths[i];
        uint32_t noff = uint32_t(strtab.size());
        strtab.insert(strtab.end(), name.begin(), name.end());
        strtab.push_back(0);
        uint64_t val = text.size(), size = w.size() * 4;
        text.insert(text.end(), (uint8_t*)w.data(), (uint8_t*)w.data() + size);
        uint8_t e[24] = {};
        memcpy(e, &noff, 4);
        e[4] = 0x12;   // GLOBAL FUNC
        uint16_t shndx = 1;
        memcpy(e + 6, &shndx, 2);
        memcpy(e + 8, &val, 8);
        memcpy(e + 16, &size, 8);
        symtab.insert(symtab.end(), e, e + 24);
    }
    static const char shstr[] = "\0.text\0.symtab\0.strtab\0.shstrtab";
    std::vector<uint8_t> f(64, 0);
    auto add = [&](const void* p, size_t s) { size_t o = f.size(); f.insert(f.end(), (const uint8_t*)p, (const uint8_t*)p + s); return o; };
    size_t ot = add(text.data(), text.size()), os = add(symtab.data(), symtab.size()), ostr = add(strtab.data(), strtab.size()),
           osh = add(shstr, sizeof shstr);
    while (f.size() % 8) f.push_back(0);
    uint64_t shoff = f.size();
    auto sh = [&](uint32_t name, uint32_t type, uint64_t flags, uint64_t off, uint64_t size, uint32_t link, uint32_t info, uint64_t align, uint64_t ent) {
        uint8_t h[64] = {};
        memcpy(h, &name, 4); memcpy(h + 4, &type, 4); memcpy(h + 8, &flags, 8); memcpy(h + 24, &off, 8);
        memcpy(h + 32, &size, 8); memcpy(h + 40, &link, 4); memcpy(h + 44, &info, 4); memcpy(h + 48, &align, 8); memcpy(h + 56, &ent, 8);
        add(h, 64);
    };
    sh(0, 0, 0, 0, 0, 0, 0, 0, 0);
    sh(1, 1, 6, ot, text.size(), 0, 0, 4, 0);
    sh(7, 2, 0, os, symtab.size(), 3, 1, 8, 24);
    sh(15, 3, 0, ostr, strtab.size(), 0, 0, 1, 0);
    sh(23, 3, 0, osh, sizeof shstr, 0, 0, 1, 0);
    uint8_t eh[64] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    uint16_t v16;
    v16 = 1; memcpy(eh + 16, &v16, 2);       // ET_REL
    v16 = 224; memcpy(eh + 18, &v16, 2);     // EM_AMDGPU
    uint32_t v32 = 1; memcpy(eh + 20, &v32, 4);
    memcpy(eh + 0x28, &shoff, 8);
    v32 = 0x36; memcpy(eh + 0x30, &v32, 4); // EF_AMDGPU_MACH (informational)
    v16 = 64; memcpy(eh + 0x34, &v16, 2);
    v16 = 64; memcpy(eh + 0x3a, &v16, 2);
    v16 = 5; memcpy(eh + 0x3c, &v16, 2);
    v16 = 4; memcpy(eh + 0x3e, &v16, 2);
    memcpy(f.data(), eh, 64);
    std::ofstream(out, std::ios::binary).write((const char*)f.data(), f.size());
    return 0;
}

// --code <file>: replace the container's machine code (after the hash markers) with raw
// instructions, keeping its header and binding records (hand-assembled ISA tests).
static bool splice_code(Container& c, const std::vector<uint8_t>& code) {
    if (!c.has_native || code.size() % 4) return false;
    std::vector<uint8_t> p(c.precompiled.begin(), c.precompiled.begin() + c.body_begin);
    p.insert(p.end(), code.begin(), code.end());
    size_t end = p.size();
    p.insert(p.end(), c.precompiled.begin() + c.code_end, c.precompiled.end());
    c.precompiled = std::move(p);
    c.code_end = end;
    return true;
}
static int translate_cmd(const char* path, const char* rspath, const char* codepath, int dual, bool gm, bool gmt) {
    Container c;
    if (!load_cso(path, c)) return 1;
    if (codepath && !splice_code(c, read_file(codepath))) return fprintf(stderr, "%s: cannot use code %s\n", path, codepath), 1;
    std::vector<uint8_t> rsb;
    if (rspath) {
        rsb = read_file(rspath);
        // A serialized root signature is the RTS0 data itself or a DXBC container holding it.
        if (rsb.size() >= 32 && !memcmp(rsb.data(), "DXBC", 4)) {
            uint32_t count;
            memcpy(&count, rsb.data() + 28, 4);
            for (uint32_t i = 0; i < count && 32 + 4ull * (i + 1) <= rsb.size(); i++) {
                uint32_t off, sz;
                memcpy(&off, rsb.data() + 32 + 4 * i, 4);
                if (off + 8ull > rsb.size()) break;
                memcpy(&sz, rsb.data() + off + 4, 4);
                if (!memcmp(rsb.data() + off, "RTS0", 4) && off + 8ull + sz <= rsb.size()) {
                    rsb = std::vector<uint8_t>(rsb.begin() + off + 8, rsb.begin() + off + 8 + sz);
                    break;
                }
            }
        }
    } else if (auto* p = c.part("RTS0")) rsb = *p;
    RootSig rs;
    std::string err;
    if (!parse_root_signature(rsb.data(), rsb.size(), rs, &err)) return fprintf(stderr, "%s: root signature: %s\n", path, err.c_str()), 1;
    TranslateOptions o;
    if (dual >= 0) o.wave64_dual = dual != 0;
    o.global_memory = gm;
    o.global_memory_test = gmt;
    TranslateResult r = translate(c, rs, o);
    if (!r.ok) return fprintf(stderr, "%s: %s\n", path, r.error.c_str()), 1;
    printf("// profile %s, wave%u\n%s", r.profile.c_str(), r.wave_size, r.hlsl.c_str());
    return 0;
}

// rtlink <template.cso|-> <blob>...: link the collections of a ray-tracing pipeline (serialized
// DeserializeStateObjectX blobs) into one program and translate it (gpucode/rtso.h).
static int rtlink_cmd(const char* tmpl, int n, char** blobs) {
    std::vector<std::vector<uint8_t>> bs;
    for (int i = 0; i < n; i++) bs.push_back(read_file(blobs[i]));
    RtLinked l;
    std::string err;
    if (!rt_link(bs, l, &err)) return fprintf(stderr, "link: %s\n", err.c_str()), 1;
    fprintf(stderr, "image 0x%zx bytes, scheduler @0x%x, %zu traversal entries, %zu exports\n", l.image.size() * 4, l.main, l.traversal.size(), l.exports.size());
    for (auto& [name, a] : l.exports) fprintf(stderr, "  export %s @0x%llx\n", name.c_str(), (unsigned long long)a);
    for (auto& [g, s] : l.hitgroups) fprintf(stderr, "  hit group %s: closest %s, any %s, intersection %s\n", g.c_str(), s[0].c_str(), s[1].c_str(), s[2].c_str());
    for (auto& p : l.parts) fprintf(stderr, "  part 0x%x..0x%x main 0x%x%s\n", p.begin, p.end, p.main, p.raygen ? " (ray generation)" : "");
    if (const char* k = getenv("RT_PARK")) {   // one collection (index into the blobs) as a parking program
        RtSpace sp;
        std::vector<int> ids;
        for (auto& b : bs) ids.push_back(sp.add(b));
        std::string e2;
        if (!sp.program(ids.at((size_t)atoi(k)), l, &e2)) return fprintf(stderr, "program: %s\n", e2.c_str()), 1;
        fprintf(stderr, "park: pointer v%u, resume at 0x%x, main 0x%x\n", l.park_ptr_vgpr, l.resume_at, l.main);
    }
    Container c;   // "-": no template (as the layer does)
    if (strcmp(tmpl, "-") && !load_cso(tmpl, c)) return 1;
    c.kind = ProgramKind::CS;
    RootSig rs;
    RsParam p;
    p.type = ParamType::Constants;
    p.num32 = kRtRootConstants;
    rs.params.push_back(p);
    TranslateResult r = translate(c, rs, rt_translate_options(l));
    if (!r.ok) return fprintf(stderr, "%s\n", r.error.c_str()), 1;
    printf("// profile %s, wave%u\n%s", r.profile.c_str(), r.wave_size, r.hlsl.c_str());
    return 0;
}

// rtcache <outdir> <salt> <blob>...: every collection (blobs in the title's deserialization order)
// as a parking program: <outdir>/rt_<key>.hlsl, keyed like the layer's cache (salt = translator
// version + compiler option, e.g. gpucode-35-Od). Compile each to rt_<key>.dxil for the cache.
static int rtcache_cmd(const char* outdir, const char* salt, int n, char** blobs) {
    RtSpace sp;
    std::vector<int> ids;
    for (int i = 0; i < n; i++) ids.push_back(sp.add(read_file(blobs[i])));
    int written = 0;
    for (size_t c = 0; c < sp.collections(); c++) {
        RtLinked l;
        std::string err;
        if (!sp.program(int(c), l, &err)) { fprintf(stderr, "collection %zu: %s\n", c, err.c_str()); continue; }
        if (getenv("RT_STATS")) l.stats = true;   // the debug-counter variant (XSL_RT_DEBUG=1 in the layer)
        Container k;
        k.kind = ProgramKind::CS;
        RootSig rs;
        RsParam p;
        p.type = ParamType::Constants;
        p.num32 = kRtRootConstants;
        rs.params.push_back(p);
        TranslateResult r = translate(k, rs, rt_translate_options(l));
        if (!r.ok) { fprintf(stderr, "collection %zu: %s\n", c, r.error.c_str()); continue; }
        char path[1024];
        snprintf(path, sizeof path, "%s/rt_%016llx.hlsl", outdir, (unsigned long long)rt_cache_key(l, salt));
        fprintf(stderr, "collection %zu: rt_%016llx (image %zu bytes, %zu segments, main 0x%x, %zu entries)\n", c, (unsigned long long)rt_cache_key(l, salt),
                l.image.size() * 4, l.segs.size(), l.main, l.entries.size());
        FILE* f = fopen(path, "wb");
        if (!f) return fprintf(stderr, "cannot write %s\n", path), 1;
        fwrite(r.hlsl.data(), 1, r.hlsl.size(), f);
        fclose(f);
        written++;
    }
    fprintf(stderr, "%zu collections, %d programs written\n", sp.collections(), written);
    return 0;
}

static int meta_cmd(const char* path) {
    Container c;
    if (!load_cso(path, c)) return 1;
    Meta m;
    std::string err;
    if (!parse_meta(c, m, &err)) return fprintf(stderr, "%s: %s\n", path, err.c_str()), 1;
    printf("header %u dwords, rsrc2 0x%x (user SGPRs %u, tgid %d%d%d, tid comps %u, lds %u), numthreads %u %u %u\n", m.header_dwords, m.rsrc2,
           m.user_sgprs(), m.tgid_en(0), m.tgid_en(1), m.tgid_en(2), m.tid_comps(), m.lds_bytes(), m.numthreads[0], m.numthreads[1], m.numthreads[2]);
    for (auto& a : m.args) printf("  arg kind %u size %u reg %u param %u loc %u\n", a.kind, a.size, a.reg, a.param, a.loc);
    for (auto& r : m.resources) printf("  res kind 0x%x offset %u reg %u elem %u param %u loc %u dwords %u\n", r.kind, r.offset, r.reg, r.elem, r.param, r.loc, r.dwords);
    // The signatures and the stage's input/output records as stored (bytes).
    for (const char* cc : {"ISG1", "OSG1", "PSG1"})
        if (const auto* part = c.part(cc))
            for (auto& e : parse_signature(*part))
                printf("  %s %s%u: register %u mask %x type %u system value %u\n", cc, e.name.c_str(), e.index, e.reg, e.mask, e.comptype, e.sysval);
    const uint8_t* b = c.precompiled.data();
    const size_t n = c.precompiled.size();
    auto recs = [&](const char* what, size_t at, size_t size, size_t countAt) {
        if (countAt + 4 > n) return;
        uint32_t cnt;
        memcpy(&cnt, b + countAt, 4);
        for (uint32_t k = 0; k < cnt && k < 32 && at + size * (k + 1) <= n; k++) {
            printf("  %s %u:", what, k);
            for (size_t j = 0; j < size; j++) printf(" %02x", b[at + size * k + j]);
            printf("\n");
        }
    };
    const size_t E = 0x114;   // the stage data: after the program record
    if (c.kind == ProgramKind::PS) recs("pixel input", E + 0x24, 8, E + 0x124);
    if (c.kind == ProgramKind::VS) { recs("vertex input", E + 0x24, 5, E + 0xc4); recs("vertex output", E + 0xc8, 6, E + 0x188); }
    if (c.kind == ProgramKind::MS) recs("mesh output", E + 0xc8, 6, E + 0x188);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "cso")) return cso(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "translate")) {
        // translate <cso> [rootsig.bin] [--code <bin>] [--wave64-dual=0|1]
        const char *rs = nullptr, *code = nullptr;
        int dual = -1;
        bool gm = false, gmt = false;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--code") && i + 1 < argc) code = argv[++i];
            else if (!strncmp(argv[i], "--wave64-dual=", 14)) dual = atoi(argv[i] + 14);
            else if (!strcmp(argv[i], "--global-memory")) gm = true;
            else if (!strcmp(argv[i], "--global-memory-test")) gmt = true;
            else rs = argv[i];
        }
        return translate_cmd(argv[2], rs, code, dual, gm, gmt);
    }
    if (argc >= 3 && !strcmp(argv[1], "meta")) return meta_cmd(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "rtlink")) return rtlink_cmd(argv[2], argc - 3, argv + 3);
    if (argc >= 5 && !strcmp(argv[1], "rtcache")) return rtcache_cmd(argv[2], argv[3], argc - 4, argv + 4);
    if (argc >= 4 && !strcmp(argv[1], "cso2elf")) return cso2elf(argv[2], argc - 3, argv + 3);
    if (argc >= 3 && !strcmp(argv[1], "dis")) {
        auto d = read_file(argv[2]);
        return dis(d, 0, d.size() & ~size_t(3), "", false);
    }
    if (argc >= 3 && !strcmp(argv[1], "dis-elf")) return dis_elf(read_file(argv[2]));
    if (argc >= 3 && !strcmp(argv[1], "dis-cso")) {   // the instructions of a container's precompiled code
        Container c;
        if (!load_cso(argv[2], c)) return 1;
        const std::vector<uint32_t> code = c.code();
        std::vector<uint8_t> d((const uint8_t*)code.data(), (const uint8_t*)code.data() + code.size() * 4);
        return dis(d, 0, d.size(), "", false);
    }
    fprintf(stderr, "usage: xsl-gpucode cso|meta <file.cso> | translate <file.cso> [rootsig.bin] [--code <bin>] [--wave64-dual=0|1] [--global-memory]\n"
                    "       | dis <code.bin> | dis-cso <file.cso> | dis-elf <file.elf> | cso2elf <out.elf> <file.cso>...\n");
    return 2;
}
