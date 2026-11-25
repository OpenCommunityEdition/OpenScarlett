// SPDX-License-Identifier: AGPL-3.0-or-later
// Console GPU machine code -> HLSL (see translate.h for the execution model).
// Instruction semantics follow AMD's public RDNA instruction-set documentation.
#include "translate.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

#include "isa.h"

namespace gpucode {
namespace {

std::string fmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}
bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
// An instruction's name may be null (an encoding the decoder does not know).
bool starts(const char* s, const char* p) { return s && !strncmp(s, p, strlen(p)); }
bool ends(const std::string& s, const char* p) {
    size_t n = strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}
bool has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

// ---- Abstract SGPR values (resource provenance) ------------------------------------------
struct AV {
    enum K : uint8_t { Undef, Top, Const, Arg, Desc, RootLo, RootHi, Pc } k = Undef;
    uint32_t a = 0;   // Const: value; Arg: SGPR loc; Desc/RootLo/RootHi: root parameter; Pc: byte offset
    uint32_t b = 0;   // Desc: byte offset of this word in the table; RootLo: byte offset added
    uint32_t c = 0;   // RootHi: bits ORed in; Desc: 1 if modified
    uint32_t d = 0;   // Arg: 1 if a run-time offset was added; Desc: 1 + index of the load that used a run-time offset
                      // (or of the representative of loads merged at a join, see merge_desc)
    uint32_t e = 0;   // Desc with a constant offset: 1 + index of the s_load that produced it (not compared)
    bool operator==(const AV& o) const { return k == o.k && a == o.a && b == o.b && c == o.c && d == o.d; }
    bool operator!=(const AV& o) const { return !(*this == o); }
    static AV top() { return AV{Top}; }
    static AV cst(uint32_t v) { return AV{Const, v}; }
};
constexpr unsigned kNumS = 128;
constexpr uint32_t kMemBlock = 0x100;   // AV::Arg location of the memory-based argument block pointer
// SGPR values plus scalar values spilled to VGPR lanes (v_writelane/v_readlane with constant lanes).
struct SState {
    std::array<AV, kNumS> s{};
    std::map<uint32_t, AV> spill;   // key: VGPR << 8 | lane
    AV& operator[](size_t i) { return s[i]; }
    const AV& operator[](size_t i) const { return s[i]; }
};
AV merge(const AV& x, const AV& y) {
    if (x.k == AV::Undef) return y;
    if (y.k == AV::Undef) return x;
    return x == y ? x : AV::top();
}

enum class ResKind { CBuffer, RawBuf, StructBuf, TypedBuf, Image, Sampler, StaticSampler, Counter };
struct Res {
    ResKind kind;
    bool uav = false;
    uint32_t reg = 0, space = 0, stride = 0;
    uint32_t param = 0, offset = 0;     // root parameter and byte offset in its table (or root)
    uint32_t root_hi_or = 0;            // root descriptors: bits the shader ORed into word 1
    uint32_t addr_off = 0;              // root descriptors: bytes the shader added to the address
    int dim = -1;                       // images: MIMG dim
    bool need_desc = false;             // images: descriptor words are read as data (desc_<name>())
    bool cube_array = false;
    bool dyn = false;                   // array indexed at run time
    int alias = -1;                     // static access to an element of resource array `alias`
    unsigned alias_index = 0;
    int counter_of = -1;                // Counter: the structured buffer whose hidden counter this is
    unsigned array = 0;                 // array size (0: unbounded) when dyn
    bool is_uint = false, is_sint = false, cmp_sampler = false, scalar = false;
    unsigned comps = 4;
    std::string name;
};

const char* kDimName[8] = {"1D", "2D", "3D", "Cube", "1DArray", "2DArray", "2DMS", "2DMSArray"};

struct Translator {
    const Container& c;
    const RootSig& rs;
    TranslateOptions opt;
    Meta meta;
    std::vector<Instr> ins;
    std::vector<uint32_t> code;
    unsigned W = 64;
    bool wave32_sure = false;
    std::string err;
    std::vector<size_t> block_start;       // instruction indices
    std::map<uint32_t, size_t> block_of;   // byte offset -> block
    std::vector<SState> block_in;
    std::vector<Res> res;
    std::map<size_t, int> res_of;          // instruction index -> resource (srsrc)
    std::map<size_t, int> samp_of;         // instruction index -> sampler resource
    std::map<size_t, std::vector<std::string>> sval;   // instruction index -> SGPR value overrides (descriptor words)
    bool uses_lds = false, uses_quad = false, uses_wave = false;
    unsigned max_v = 0;
    std::ostringstream out;

    Translator(const Container& c_, const RootSig& rs_, const TranslateOptions& o) : c(c_), rs(rs_), opt(o) {}

    bool fail(const std::string& e) {
        if (err.empty()) err = e;
        return false;
    }

    // ---- setup ------------------------------------------------------------------------------
    std::vector<PsvResource> psv;
    std::vector<SigElem> isg, osg, psg;   // input, output, primitive (mesh per-primitive) signatures
    bool has_prims() const { for (auto& e : psg) if (e.sysval == 0) return true; return false; }
    // Pixel shaders: barycentric VGPR -> interpolation mode; console attribute -> mode used.
    std::map<unsigned, std::string> bary_mode;
    std::map<unsigned, std::string> attr_mode;
    std::map<unsigned, unsigned> attr_reg;          // console attribute N -> input signature register
    std::vector<std::pair<unsigned, std::string>> ps_sysv;   // VGPR initialisations
    std::set<unsigned> ps_targets;
    bool ps_depth = false, ps_stencil = false, ps_mask = false, ps_discard = false;
    // Vertex shaders.
    bool vs_fetch = false;
    bool uses_inert_bvh = false;   // image_bvh*_intersect_ray translated as misses
    std::map<unsigned, unsigned> vs_input_vgpr;    // input signature register -> first VGPR
    std::set<unsigned> vs_params;
    bool vs_pos = false;
    bool uses_codedata = false;
    bool uses_gm = false;   // global loads/stores through the layer's address table
    bool uses_bvh = false;  // image_bvh*_intersect_ray in HLSL over global memory
    unsigned ms_max_verts = 256, ms_max_prims = 256, ms_lds = 0;
    bool merged_vs = false;
    bool uses_clipcull = false;
    // Wave64-on-wave32 mode (compute/mesh): each PC thread runs console lanes L and L+32 (copy 0/1).
    bool dual = false, want_dual = false;
    int cp = -1;   // vertex shader compiled as a merged (ES/GS or primitive) stage
    unsigned user_end() const {   // first SGPR after the user arguments (PS: primitive mask)
        unsigned e = c.kind == ProgramKind::CS ? 4 : 8;
        for (auto& a : meta.args)
            if (a.loc < 32) e = std::max(e, a.loc + std::max(1u, a.size));
        return e;
    }
    const PsvResource* psv_find(bool sampler, bool cbv, bool uav, uint32_t reg, uint32_t space) const {
        for (auto& p : psv) {
            bool cls = sampler ? p.type == 1 : cbv ? p.type == 2 : uav ? (p.type >= 6 && p.type <= 9) : (p.type >= 3 && p.type <= 5);
            if (cls && p.space == space && reg >= p.lo && reg <= p.hi) return &p;
        }
        return nullptr;
    }
    // Refines a resource's kind and image dimension from PSV0 when the container has it.
    void refine(Res& r) const {
        const PsvResource* p = psv_find(r.kind == ResKind::Sampler, r.kind == ResKind::CBuffer, r.uav, r.reg, r.space);
        if (!p || !p->kind) return;
        static const int kdim[10] = {-1, 0, 1, 6, 2, 3, 4, 5, 7, 3};
        if (p->kind >= 1 && p->kind <= 9) { r.kind = ResKind::Image; r.dim = kdim[p->kind]; r.cube_array = p->kind == 9; }
        else if (p->kind == 10) r.kind = ResKind::TypedBuf;
        else if (p->kind == 11) r.kind = ResKind::RawBuf;
        else if (p->kind == 12 && r.kind != ResKind::StructBuf) { r.kind = ResKind::StructBuf; if (!r.stride) r.stride = 4; }
    }
    bool load() {
        std::string e;
        if (auto* pp = c.part("PSV0")) psv = parse_psv0(*pp);
        if (auto* pp = c.part("ISG1")) isg = parse_signature(*pp);
        if (auto* pp = c.part("OSG1")) osg = parse_signature(*pp);
        if (auto* pp = c.part("PSG1")) psg = parse_signature(*pp);
        if (!opt.raw_code.empty()) {   // raw program: metadata from the options
            meta = Meta{};
            meta.valid = true;
            meta.rsrc2 = opt.raw_rsrc2;
            meta.numthreads[0] = opt.raw_numthreads;
            if (opt.raw_root_constants) meta.args.push_back(UserArg{0, opt.raw_root_constants, 0, 0, 0});
        } else if (!parse_meta(c, meta, &e)) return fail("metadata: " + e);
        if (c.kind == ProgramKind::MS) {
            // Mesh shaders run as merged (primitive) shaders: argument locations are 8 SGPRs higher.
            for (auto& a : meta.args) if (a.loc < 32) a.loc += 8;
            for (auto& r : meta.resources) if (r.loc < 32) r.loc += 8;
            if (auto* pp = c.part("PSV0"); pp && pp->size() >= 4 + 48) {
                auto rd = [&](size_t o) { uint32_t v; memcpy(&v, pp->data() + 4 + o, 4); return v; };
                ms_lds = rd(0);
                ms_max_verts = rd(12) & 0xffff;
                ms_max_prims = rd(12) >> 16;
                for (int k = 0; k < 3; k++) meta.numthreads[k] = rd(36 + 4 * k);
            }
            // PC mesh shaders have at most 128 threads: larger console groups run two console lanes
            // per PC thread (wave64 on wave32).
            unsigned nt = meta.numthreads[0] * meta.numthreads[1] * meta.numthreads[2];
            if (nt > 128) want_dual = true;
        }
        if (c.kind == ProgramKind::PS) {
            // Initial VGPRs in SPI_PS_INPUT_ENA order (AMD's public register layout).
            static const struct { int bit, n; const char* what; } lay[] = {
                {0, 2, "sample"}, {1, 2, "center"}, {2, 2, "centroid"}, {3, 3, "pull"}, {4, 2, "nsample"}, {5, 2, "ncenter"},
                {6, 2, "ncentroid"}, {7, 1, "stipple"}, {8, 1, "posx"}, {9, 1, "posy"}, {10, 1, "posz"}, {11, 1, "posw"},
                {12, 1, "face"}, {13, 1, "ancillary"}, {14, 1, "coverage"}, {15, 1, "fixed"}};
            unsigned v = 0;
            for (auto& l : lay) {
                if (!((meta.ps_input_ena >> l.bit) & 1)) continue;
                for (int k = 0; k < l.n; k++) bary_mode[v + k] = l.what;
                std::string w = l.what, init;
                if (w == "posx") init = "asuint(I.pos_.x)";
                else if (w == "posy") init = "asuint(I.pos_.y)";
                else if (w == "posz") init = "asuint(I.pos_.z)";
                else if (w == "posw") init = "asuint(1.0f / I.pos_.w)";   // the hardware supplies 1/w (D3D SV_Position.w is w)
                else if (w == "face") init = "(I.face_ ? 0x3f800000u : 0xbf800000u)";
                else if (w == "ancillary") {   // [11:8] sample index, [28:16] render-target array index
                    init = "((I.sid_ & 15u) << 8)";
                    for (auto& e : isg)
                        if (e.sysval == 4) init = fmt("(((I.sid_ & 15u) << 8) | ((I.sv%u_ & 0x1fffu) << 16))", e.reg);
                }
                else if (w == "coverage") init = "I.cov_";
                if (!init.empty()) ps_sysv.push_back({v, init});
                v += l.n;
            }
            for (size_t k = 0; k < meta.ps_inputs.size(); k++) attr_reg[unsigned(k)] = meta.ps_inputs[k].reg;
            // (The records name their attribute; they are in attribute order in every program seen.)
            for (size_t k = 0; k < meta.ps_inputs.size(); k++)
                if (meta.ps_inputs[k].attr < 64 && meta.ps_inputs[k].attr != k) attr_reg[meta.ps_inputs[k].attr] = meta.ps_inputs[k].reg;
        }
        code = opt.raw_code.empty() ? c.code() : opt.raw_code;
        ins = decode_all(code.data(), code.size());
        if (!opt.raw_skip.empty()) {
            std::vector<std::pair<uint32_t, uint32_t>> sk = opt.raw_skip;
            std::sort(sk.begin(), sk.end());
            std::vector<Instr> kept;
            kept.reserve(ins.size());
            for (auto& in : ins) {
                auto it = std::upper_bound(sk.begin(), sk.end(), std::make_pair(in.offset, 0xffffffffu));
                bool skip = false;
                for (auto k = it; k != sk.begin() && !skip;) {   // ranges may overlap: check those starting at or before the offset
                    --k;
                    skip = in.offset < k->second;
                    if (it - k > 8) break;
                }
                if (!skip) kept.push_back(in);
            }
            ins.swap(kept);
        }
        for (auto& in : ins) {
            if (!in.name) continue;   // data after the code decodes as junk; reported only if reached
            std::string n = in.name;
            if ((n == "s_mov_b32" || starts(n, "s_and_b32") || starts(n, "s_or_b32")) && (in.dst == 126))
                W = 32;
            // only wave32 code saves EXEC in one register: that decides it. (Wave32 code may still use
            // 64-bit forms on the EXEC pair, e.g. s_or_saveexec_b64 around whole-wave sections of the
            // ray-traversal code; read as wave64, every compare result would clobber a second register.)
            if (n == "s_and_saveexec_b32" || n == "s_or_saveexec_b32" || n == "s_andn2_wrexec_b32" ||
                n == "s_xor_saveexec_b32" || n == "s_andn2_saveexec_b32") {
                W = 32;
                wave32_sure = true;
            }
        }
        for (auto& in : ins) {
            if (!in.name || wave32_sure) continue;
            std::string n = in.name;
            if (ends(n, "_b64") && (in.dst == 126 || in.src0 == 126 || in.src1 == 126) && in.fmt != Fmt::VOP3) W = 64;
            if (has(n, "saveexec_b64") || has(n, "wrexec_b64")) W = 64;
        }
        if (c.wave_hint) W = c.wave_hint;   // the container says (its code may use 64-bit forms on a pair in wave32 code)
        if (c.kind == ProgramKind::CS && W == 64 && opt.wave64_dual) {
            unsigned nt = meta.numthreads[0] * meta.numthreads[1] * meta.numthreads[2];
            if (nt % 64 == 0) dual = true;   // partial console waves: single-lane mapping (approximate)
        }
        if (want_dual) {
            unsigned nt = meta.numthreads[0] * meta.numthreads[1] * meta.numthreads[2];
            if (W != 64 || nt % 64 || nt > 256) return fail(fmt("mesh shader with %u threads (wave%u) not supported", nt, W));
            dual = true;
        }
        return true;
    }

    bool jumped = false;     // the block's last instruction set pc (indirect jump)
    std::set<uint32_t> jump_targets;   // offsets reached by indirect jumps (entries, return sites)
    bool indirect = false;   // s_setpc_b64 / s_swappc_b64 (not the VS fetch-shader call): jumps through jump_case()
    void build_cfg() {
        std::set<uint32_t> leaders{0};
        for (uint32_t e : opt.entries) { leaders.insert(e); jump_targets.insert(e); }
        if (!opt.raw_code.empty()) leaders.insert(opt.raw_entry);
        for (size_t i = 0; i < ins.size(); i++) {
            auto& in = ins[i];
            if (in.name && (!strcmp(in.name, "s_setpc_b64") || !strcmp(in.name, "s_swappc_b64")) &&
                !(!strcmp(in.name, "s_swappc_b64") && c.kind == ProgramKind::VS && i <= 1 && in.src0 == 4)) {
                indirect = true;
                leaders.insert(in.offset + 4);   // the return address (s_swappc) or what follows a jump
                jump_targets.insert(in.offset + 4);
            }
            if (in.fmt != Fmt::SOPP || !in.name) continue;
            std::string n = in.name;
            if (starts(n, "s_branch") || starts(n, "s_cbranch")) {
                leaders.insert(uint32_t(int32_t(in.offset) + 4 + in.simm16 * 4));
                leaders.insert(in.offset + 4);
            } else if (n == "s_endpgm") {
                leaders.insert(in.offset + 4);
            } else if (n == "s_barrier") {
                leaders.insert(in.offset + 4);   // (code with loops stops at a barrier and goes on from the next block)
            }
        }
        for (size_t i = 0; i < ins.size(); i++)
            if (leaders.count(ins[i].offset)) {
                block_of[ins[i].offset] = block_start.size();
                block_start.push_back(i);
            }
    }
    size_t block_end(size_t b) const { return b + 1 < block_start.size() ? block_start[b + 1] : ins.size(); }

    // ---- provenance -------------------------------------------------------------------------
    const UserArg* arg_at(uint32_t loc) const {
        for (auto& a : meta.args)
            if (a.loc == loc) return &a;
        return nullptr;
    }
    AV sval_of(const SState& st, uint16_t enc, bool hi = false) const {
        if (enc < kNumS) return st[enc];
        if (enc >= src::kZero && enc <= src::kIntPos64) return AV::cst(hi ? 0 : enc - src::kZero);
        if (enc >= src::kIntNeg1 && enc <= src::kIntNeg16) return AV::cst(hi ? 0xffffffffu : uint32_t(-(int(enc) - 192)));
        return AV::top();
    }
    uint32_t code_dword(uint32_t byte_off) const {   // byte offset relative to the code start
        size_t at = c.body_begin + byte_off;
        if (at + 4 > c.precompiled.size()) return 0;
        uint32_t v;
        memcpy(&v, c.precompiled.data() + at, 4);
        return v;
    }
    size_t cur_idx = 0;
    std::set<size_t> dyn_loads;            // s_load instructions whose offset is known only at run time
    // Descriptor loads from one table at different constant offsets that meet at a join (a shader
    // picking one of several descriptors by branch): the loads of a group write their offset into
    // the representative's dyn variable, and the use indexes the table range with it.
    std::set<size_t> gm_sloads;            // s_load from run-time pointers: read through global memory
    std::set<size_t> dyn_mubuf;
    std::set<size_t> dyn_sbuf;
    bool uses_fmt_comp = false;            // fmt_comp(): typed buffer components from a run-time format
    bool uses_fmt_put = false;             // fmt_put(): the same for stores
    std::set<size_t> dyn_mimg;             // image loads/stores on descriptors from descriptor memory
    std::set<size_t> dyn_samp;             // samplers read from descriptor memory (layer payload: sampler-heap index)
    bool uses_heap_sampler = false, uses_heap_sampler_cmp = false;
    bool uses_dot4 = false;   // dot4_i8(): the 8-bit dot products
    bool uses_heap_cube = false;
    bool uses_park = false;   // ray-tracing program parking (TranslateOptions::raw_park)   // cube textures sampled through descriptor memory (space 919)
    bool uses_gm_atomic = false;
    // 32-bit buffer atomics global memory supports: operation code for gm_atomic(), 0 = none.
    // Byte/short buffer accesses: 1 store_byte 2 store_short 3 load_ubyte 4 load_sbyte 5 load_ushort 6 load_sshort.
    static unsigned dyn_small_op(const std::string& n) {
        static const char* ops[] = {"buffer_store_byte", "buffer_store_short", "buffer_load_ubyte", "buffer_load_sbyte", "buffer_load_ushort", "buffer_load_sshort"};
        for (unsigned k = 0; k < 6; k++)
            if (n == ops[k]) return k + 1;
        return 0;
    }
    static unsigned dyn_atomic_op(const std::string& n) {
        static const char* ops[] = {"buffer_atomic_add", "buffer_atomic_sub", "buffer_atomic_umin", "buffer_atomic_umax", "buffer_atomic_smin",
                                    "buffer_atomic_smax", "buffer_atomic_and", "buffer_atomic_or", "buffer_atomic_xor", "buffer_atomic_swap"};
        for (unsigned k = 0; k < 10; k++)
            if (n == ops[k]) return k + 1;
        return 0;
    }
    std::set<unsigned> heap_dims;          // MIMG dims used by those (bindless heap arrays to declare)             // s_buffer_load whose descriptor is known only at run time            // raw buffer dword accesses whose descriptor is known only at run time
    bool soft_trace = false;
    std::map<size_t, size_t> dyn_alias;    // load -> representative load
    std::set<size_t> merged_reps;
    std::map<size_t, std::pair<uint32_t, uint32_t>> merged_span;   // representative -> lowest, highest slot byte offset
    AV merge_desc(const AV& x, const AV& y) {
        if (x.k != AV::Desc || y.k != AV::Desc || x == y || x.a != y.a || x.c != y.c) return merge(x, y);
        auto rep = [&](const AV& v) -> long {
            if (v.d) return merged_reps.count(v.d - 1) ? long(v.d - 1) : -1;
            return v.e ? long(v.e - 1) : -1;
        };
        long rx = rep(x), ry = rep(y);
        uint32_t wx = x.d ? x.b : x.b % 32, wy = y.d ? y.b : y.b % 32;   // byte within the 32-byte slot
        if (rx < 0 || ry < 0 || wx != wy) return merge(x, y);
        size_t r = size_t(std::min(rx, ry)), o = size_t(std::max(rx, ry));
        auto span = [&](const AV& v, long rp) {
            if (v.d) return merged_span.count(size_t(rp)) ? merged_span[size_t(rp)] : std::make_pair(0u, 0u);
            uint32_t b0 = v.b - v.b % 32;
            return std::make_pair(b0, b0);
        };
        auto sx = span(x, rx), sy = span(y, ry);
        merged_span[r] = {std::min(sx.first, sy.first), std::max(sx.second, sy.second)};
        if (o != r) merged_span.erase(o);
        for (auto& [ld, rp] : dyn_alias)
            if (rp == o) rp = r;
        dyn_alias[size_t(rx)] = r;
        dyn_alias[size_t(ry)] = r;
        merged_reps.erase(o);
        merged_reps.insert(r);
        AV m{AV::Desc, x.a, wx, x.c, uint32_t(r + 1)};
        return m;
    }
    std::map<size_t, std::string> res_index;   // instruction -> array index expression (resource arrays)
    std::map<size_t, std::string> samp_index;
    std::string rn(size_t idx, int r) const {   // resource name at an instruction (indexed when an array)
        if (res[r].alias >= 0) return fmt("%s[%u]", res[res[r].alias].name.c_str(), res[r].alias_index);
        if (!res[r].dyn) return res[r].name;
        auto it = res_index.find(idx);
        return res[r].name + "[" + (it == res_index.end() ? std::string("0") : it->second) + "]";
    }
    std::string sn(size_t idx, int r) const {
        if (res[r].alias >= 0) return fmt("%s[%u]", res[res[r].alias].name.c_str(), res[r].alias_index);
        if (!res[r].dyn) return res[r].name;
        auto it = samp_index.find(idx);
        return res[r].name + "[" + (it == samp_index.end() ? std::string("0") : it->second) + "]";
    }
    void step(SState& st, const Instr& in) {
        if (!in.name) return;
        std::string n = in.name;
        auto lit = [&](uint16_t e) -> AV { return e == src::kLiteral ? AV::cst(in.literal) : sval_of(st, e); };
        auto setd = [&](uint16_t d, AV v) { if (d < kNumS) st[d] = v; };
        switch (in.fmt) {
        case Fmt::SOP1: {
            if (n == "s_mov_b32") setd(in.dst, lit(in.src0));
            else if (n == "s_mov_b64") {
                AV lo = lit(in.src0), hi = in.src0 < kNumS ? sval_of(st, in.src0 + 1) : (in.src0 == src::kLiteral ? AV::cst(0) : sval_of(st, in.src0, true));
                setd(in.dst, lo);
                setd(in.dst + 1, hi);
            } else if (n == "s_bitset1_b32" || n == "s_bitset0_b32") {
                AV d = st[in.dst < kNumS ? in.dst : 0], k = lit(in.src0);
                if (k.k == AV::Const && (d.k == AV::Arg || d.k == AV::RootHi)) {
                    AV r = d.k == AV::Arg ? AV{AV::RootHi, d.a, 0, 0} : d;
                    if (n == "s_bitset1_b32") r.c |= 1u << (k.a & 31);
                    setd(in.dst, r);
                } else if (k.k == AV::Const && d.k == AV::Const) {
                    setd(in.dst, AV::cst(n == "s_bitset1_b32" ? d.a | (1u << (k.a & 31)) : d.a & ~(1u << (k.a & 31))));
                } else if (d.k == AV::Desc) {
                    AV r = d; r.c = 1; setd(in.dst, r);
                } else setd(in.dst, AV::top());
            } else if (n == "s_getpc_b64") {
                setd(in.dst, AV{AV::Pc, in.offset + 4});
                setd(in.dst + 1, AV::top());
            } else {
                setd(in.dst, AV::top());
                if (wide_dst(n) || has(n, "saveexec_b64")) setd(in.dst + 1, AV::top());
            }
            break;
        }
        case Fmt::SOP2: {
            AV x = lit(in.src0), y = lit(in.src1), r = AV::top();
            if (n == "s_add_u32" || n == "s_add_i32") {
                if (y.k == AV::Const && (x.k == AV::Arg || x.k == AV::RootLo || x.k == AV::Pc)) r = AV{x.k, x.a, x.b + y.a, x.c, x.d};
                else if (x.k == AV::Const && (y.k == AV::Arg || y.k == AV::RootLo || y.k == AV::Pc)) r = AV{y.k, y.a, y.b + x.a, y.c, y.d};
                else if (x.k == AV::Const && y.k == AV::Const) r = AV::cst(x.a + y.a);
                else if (x.k == AV::Arg && y.k != AV::Arg) r = AV{AV::Arg, x.a, x.b, x.c, 1};   // table pointer + run-time offset
                else if (y.k == AV::Arg && x.k != AV::Arg) r = AV{AV::Arg, y.a, y.b, y.c, 1};
            } else if (n == "s_addc_u32") {
                if (y.k == AV::Const && y.a == 0) r = x;
            } else if (n == "s_or_b32" || n == "s_and_b32" || n == "s_andn2_b32" || n == "s_xor_b32") {
                AV d = x.k == AV::Const ? y : x, k = x.k == AV::Const ? x : y;
                if (x.k == AV::Const && y.k == AV::Const) {
                    uint32_t v = n == "s_or_b32" ? (x.a | y.a) : n == "s_and_b32" ? (x.a & y.a) : n == "s_xor_b32" ? (x.a ^ y.a) : (x.a & ~y.a);
                    r = AV::cst(v);
                } else if (k.k == AV::Const && (d.k == AV::Desc)) {
                    r = d;
                    r.c = 1;
                } else if (k.k == AV::Const && (d.k == AV::RootHi || d.k == AV::Arg)) {
                    // Address high word with stride/flag bits ORed in (root descriptors).
                    r = d.k == AV::Arg ? AV{AV::RootHi, d.a, 0, 0} : d;
                    if (n == "s_or_b32") r.c |= k.a;
                } else if (d.k == AV::Arg && k.k == AV::Const && n == "s_and_b32") {
                    r = d;
                }
            } else if (n == "s_lshl_b32" && x.k == AV::Const && y.k == AV::Const) {
                r = AV::cst(x.a << (y.a & 31));
            } else if (n == "s_lshr_b32" && x.k == AV::Const && y.k == AV::Const) {
                r = AV::cst(x.a >> (y.a & 31));
            } else if (n == "s_mul_i32" && x.k == AV::Const && y.k == AV::Const) {
                r = AV::cst(x.a * y.a);
            } else if (n == "s_sub_u32" && x.k == AV::Const && y.k == AV::Const) {
                r = AV::cst(x.a - y.a);
            } else if (n == "s_bfm_b32" && x.k == AV::Const && y.k == AV::Const) {
                // (a run of ones: how code writes the level range of a sampler it builds in place)
                r = AV::cst(((1u << (x.a & 31)) - 1) << (y.a & 31));
            } else if (n == "s_bfm_b64" && x.k == AV::Const && y.k == AV::Const) {
                const uint64_t m = ((x.a & 63) ? (~0ull >> (64 - (x.a & 63))) : 0) << (y.a & 63);
                setd(in.dst, AV::cst(uint32_t(m)));
                setd(in.dst + 1, AV::cst(uint32_t(m >> 32)));
                break;
            }
            setd(in.dst, r);
            if (wide_dst(n) || n == "s_bfe_u64" || n == "s_bfe_i64") setd(in.dst + 1, AV::top());
            break;
        }
        case Fmt::SOPK: {
            if (n == "s_movk_i32") setd(in.dst, AV::cst(uint32_t(in.simm16)));
            else if (starts(n, "s_cmpk") || n == "s_setreg_b32" || n == "s_setreg_imm32_b32") {}
            else if (n == "s_bitset1_b32" || n == "s_bitset0_b32") {}
            else setd(in.dst, AV::top());
            break;
        }
        case Fmt::SMEM: {
            unsigned cnt = has(n, "x16") ? 16 : has(n, "x8") ? 8 : has(n, "x4") ? 4 : has(n, "x2") ? 2 : 1;
            AV base = st[in.sbase];
            int32_t off = in.offset_imm;
            AV so = in.soffset == src::kNull ? AV::cst(0) : sval_of(st, in.soffset);
            bool soc = so.k == AV::Const;
            for (unsigned i = 0; i < cnt; i++) {
                AV v = AV::top();
                if (starts(n, "s_load_dword") && base.k == AV::Arg && base.a == kMemBlock && soc) {
                    uint32_t dw = (uint32_t(off) + so.a + base.b) / 4 + i;
                    for (auto& a : meta.args)
                        if (a.loc >= 32 && dw >= a.loc - 32 && dw < a.loc - 32 + std::max(1u, a.size) && a.kind != 0)
                            v = AV{AV::Arg, a.loc + (dw - (a.loc - 32))};
                } else if (starts(n, "s_load_dword") && base.k == AV::Arg) {
                    // Table pointers are 0 in the generated code, so a run-time offset is the SGPR's value.
                    uint32_t at = uint32_t(off) + (soc ? so.a : 0) + 4 * i;
                    const UserArg* a = arg_at(base.a);
                    bool dyn = base.d || !soc;
                    if (a && a->kind == 3) {
                        v = AV{AV::Desc, a->param, base.b + at, 0, dyn ? uint32_t(cur_idx + 1) : 0};
                        if (!dyn && !base.d) v.e = uint32_t(cur_idx + 1);
                    }
                } else if (starts(n, "s_load_dword") && soc) {
                    uint32_t at = uint32_t(off) + so.a + 4 * i;
                    if (base.k == AV::Pc) {
                        v = AV::cst(code_dword(base.b + at));
                    }
                }
                setd(in.sdata + i, v);
            }
            break;
        }
        case Fmt::VOP1: case Fmt::VOP2: case Fmt::VOP3: case Fmt::VOPC: case Fmt::VOP3P: {
            if (n == "v_writelane_b32" || n == "v_readlane_b32") {
                AV lane = in.src1 == src::kLiteral ? AV::cst(in.literal) : sval_of(st, in.src1);
                if (n == "v_writelane_b32") {
                    if (lane.k == AV::Const) st.spill[uint32_t(in.dst) << 8 | (lane.a & 63)] = lit(in.src0);
                    else for (auto it = st.spill.begin(); it != st.spill.end();) it = (it->first >> 8) == in.dst ? st.spill.erase(it) : std::next(it);
                } else {
                    AV v = AV::top();
                    if (lane.k == AV::Const && in.src0 >= src::kVgpr0) {
                        auto it = st.spill.find(uint32_t(in.src0 - src::kVgpr0) << 8 | (lane.a & 63));
                        if (it != st.spill.end()) v = it->second;
                    }
                    setd(in.dst, v);
                }
            } else if (n == "v_readfirstlane_b32") setd(in.dst, AV::top());
            if (n == "v_movreld_b32" || starts(n, "v_movrelsd")) st.spill.clear();   // indexed write: any VGPR
            // Any other write to a VGPR invalidates scalar values spilled to it.
            if ((in.fmt == Fmt::VOP1 || in.fmt == Fmt::VOP2 || (in.fmt == Fmt::VOP3 && in.op >= 256) || in.fmt == Fmt::VOP3P) &&
                n != "v_writelane_b32" && n != "v_readlane_b32" && n != "v_readfirstlane_b32" && !st.spill.empty())
                for (auto it = st.spill.begin(); it != st.spill.end();) it = (it->first >> 8) == in.dst ? st.spill.erase(it) : std::next(it);
            // Lane masks are one SGPR in wave32 code, a pair in wave64 code.
            if (in.fmt == Fmt::VOP3 && in.op < 256) {            // compare: mask in the vdst field
                setd(in.dst, AV::top());
                if (W == 64) setd(in.dst + 1, AV::top());
            } else if (in.fmt == Fmt::VOP3 && (has(n, "_co_") || starts(n, "v_div_scale") || starts(n, "v_mad_u64") || starts(n, "v_mad_i64"))) {
                setd(in.sdst, AV::top());
                if (W == 64) setd(in.sdst + 1, AV::top());
            }
            if (in.fmt == Fmt::VOPC || (in.fmt == Fmt::VOP2 && has(n, "_co_"))) {
                st[106] = AV::top();
                if (W == 64) st[107] = AV::top();
            }
            break;
        }
        default:
            break;
        }
    }
    void analyse() {
        block_in.assign(block_start.size(), SState{});
        SState init{};
        for (unsigned i = 0; i < kNumS; i++) init[i] = AV::top();
        for (auto& a : meta.args)
            if (a.loc < 32)
                for (unsigned k = 0; k < std::max(1u, a.size); k++) init[a.loc + k] = AV{AV::Arg, a.loc + k};
        for (unsigned i = 0; i < meta.user_sgprs() && i < 32; i++)
            if (init[i].k == AV::Top) init[i] = AV{AV::Arg, i};
        // Memory-based argument ABI (argument locations from 32): the block pointer arrives in
        // s[0:1] (s[8:9] for merged stages).
        bool memabi = false;
        for (auto& a : meta.args) memabi |= a.loc >= 32;
        if (memabi) {
            unsigned b = (c.kind == ProgramKind::MS || merged_vs) ? 8 : 0;
            init[b] = AV{AV::Arg, kMemBlock};
            init[b + 1] = AV{AV::Arg, kMemBlock + 1};
        }
        std::vector<bool> seen(block_start.size(), false);
        size_t entry = !opt.raw_code.empty() && block_of.count(opt.raw_entry) ? block_of[opt.raw_entry] : 0;
        if (indirect) {   // targets of indirect jumps start with nothing known (their callers vary)
            SState top{};
            for (unsigned i = 0; i < kNumS; i++) top[i] = AV::top();
            for (uint32_t t : jump_targets)
                if (block_of.count(t)) { block_in[block_of[t]] = top; seen[block_of[t]] = true; }
        }
        block_in[entry] = init;
        seen[entry] = true;
        for (int iter = 0; iter < 64; iter++) {
            bool changed = false;
            for (size_t b = 0; b < block_start.size(); b++) {
                if (!seen[b]) continue;
                SState st = block_in[b];
                size_t e = block_end(b);
                for (size_t i = block_start[b]; i < e; i++) { cur_idx = i; step(st, ins[i]); }
                // successors
                const Instr& last = ins[e - 1];
                std::vector<size_t> succ;
                std::string ln = last.name;
                bool uncond = last.fmt == Fmt::SOPP && starts(ln, "s_branch");
                bool end = last.fmt == Fmt::SOPP && ln == "s_endpgm";
                if (last.fmt == Fmt::SOPP && (starts(ln, "s_branch") || starts(ln, "s_cbranch"))) {
                    auto it = block_of.find(uint32_t(int32_t(last.offset) + 4 + last.simm16 * 4));
                    if (it != block_of.end()) succ.push_back(it->second);
                }
                if (!uncond && !end && b + 1 < block_start.size()) succ.push_back(b + 1);
                for (size_t s : succ) {
                    SState m = block_in[s];
                    bool ch = !seen[s];
                    for (unsigned k = 0; k < kNumS; k++) {
                        AV v = seen[s] ? merge_desc(m[k], st[k]) : st[k];
                        if (!(v == m[k])) {
                            m[k] = v;
                            ch = true;
                        }
                    }
                    if (!seen[s]) {
                        if (m.spill != st.spill) { m.spill = st.spill; ch = true; }
                    } else {
                        for (auto it = m.spill.begin(); it != m.spill.end();) {
                            auto o = st.spill.find(it->first);
                            AV v = o == st.spill.end() ? AV::top() : merge(it->second, o->second);
                            if (!(v == it->second)) { ch = true; if (v.k == AV::Top) { it = m.spill.erase(it); continue; } it->second = v; }
                            ++it;
                        }
                    }
                    seen[s] = true;
                    if (ch) {
                        block_in[s] = m;
                        changed = true;
                    }
                }
            }
            if (!changed) break;
        }
        // Resolve resources with the converged states (static accesses into a run-time-indexed
        // range then become elements of that array).
        for (size_t b = 0; b < block_start.size(); b++) {
            if (!seen[b]) continue;
            SState st = block_in[b];
            for (size_t i = block_start[b]; i < block_end(b); i++) {
                cur_idx = i;
                resolve(i, st);
                step(st, ins[i]);
            }
        }
        infer_typed_components();
    }

    // Typed buffers and images: HLSL needs a component type, which on the console is the view's
    // format (unknown here). Values a shader stores or loads are mostly produced or consumed by
    // integer or float ALU code; the majority decides (no evidence: integer for buffers, float for images).
    // 64-bit scalar ops whose destination is a pair (s_bcnt/s_ff/s_flbit *_i32_b64 write 32 bits;
    // compares and s_bitcmp only SCC).
    static bool wide_dst(const std::string& n) {
        if (!ends(n, "_b64")) return false;
        return !(has(n, "_i32_b64") || has(n, "_u32_b64") || starts(n, "s_bitcmp") || starts(n, "s_cmp"));
    }
    static bool valu(const Instr& x) {
        return x.name && (x.fmt == Fmt::VOP1 || x.fmt == Fmt::VOP2 || x.fmt == Fmt::VOP3P || (x.fmt == Fmt::VOP3 && x.op >= 256)) &&
               strncmp(x.name, "v_cmp", 5) && strcmp(x.name, "v_readlane_b32") && strcmp(x.name, "v_readfirstlane_b32");
    }
    static int op_kind(const char* nm) {   // +1 integer, -1 float, 0 data movement
        std::string n = nm;
        if (starts(n, "v_cvt_f32_") || starts(n, "v_cvt_f16_") || starts(n, "v_cvt_pk")) return -1;
        if (starts(n, "v_cvt_")) return 1;
        if (n == "v_mov_b32" || n == "v_cndmask_b32" || starts(n, "v_mov_b32")) return 0;
        if (has(n, "_f32") || has(n, "_f16") || has(n, "_f64")) return -1;
        return 1;
    }
    int const_kind(uint16_t src, uint32_t lit) {
        if (src >= 129 && src <= 208) return 1;                // inline integers (0 is neutral)
        if (src >= 240 && src <= 248) return -1;               // inline floats
        if (src == src::kLiteral) {
            uint32_t e = (lit >> 23) & 0xff;
            return lit != 0 && e >= 100 && e <= 160 ? -1 : 1;  // plausible float magnitude
        }
        return 0;
    }
    int producer_kind(size_t at, unsigned reg, int depth) {
        for (size_t j = at; j-- > 0 && at - j < 256;) {
            const Instr& x = ins[j];
            if (!valu(x) || x.dst != reg) continue;
            int k = op_kind(x.name);
            if (k || depth > 3) return k;
            uint16_t srcs[2] = {x.src0, x.src1};
            int v = 0;
            for (int q = 0; q < (strcmp(x.name, "v_cndmask_b32") ? 1 : 2); q++) {
                uint16_t sv = srcs[q];
                if (sv >= src::kVgpr0) v += producer_kind(j, sv - src::kVgpr0, depth + 1);
                else v += const_kind(sv, x.literal);
            }
            return v > 0 ? 1 : v < 0 ? -1 : 0;
        }
        return 0;
    }
    int consumer_kind(size_t at, unsigned reg) {
        for (size_t j = at + 1; j < ins.size() && j - at < 256; j++) {
            const Instr& x = ins[j];
            if (!valu(x)) continue;
            uint16_t r = uint16_t(src::kVgpr0 + reg);
            if (x.src0 == r || x.src1 == r || x.src2 == r) {
                int k = op_kind(x.name);
                if (k) return k;
            }
            if (x.dst == reg) return 0;   // overwritten before a typed use
        }
        return 0;
    }
    void infer_typed_components() {
        std::map<int, int> vote;
        for (auto& [i, r] : res_of) {
            if (r < 0) continue;
            Res& R = res[r];
            if ((R.kind != ResKind::TypedBuf && R.kind != ResKind::Image) || R.is_uint) continue;
            const Instr& in = ins[i];
            std::string n = in.name;
            bool store = has(n, "store"), load = has(n, "load");
            if (starts(n, "image_sample") || starts(n, "image_gather") || (!store && !load)) continue;
            unsigned cnt = in.fmt == Fmt::MIMG ? unsigned(__builtin_popcount(in.dmask)) : ends(n, "_xyzw") ? 4 : ends(n, "_xyz") ? 3 : ends(n, "_xy") ? 2 : 1;
            if (in.d16) continue;
            for (unsigned k = 0; k < cnt; k++) vote[r] += store ? producer_kind(i, in.vdata + k, 0) : consumer_kind(i, in.vdata + k);
        }
        // Typed buffers only moved through (constants, raw loads, zeros) stay integers: indirect
        // arguments and counters are written that way. Images keep float without evidence, and
        // an image that is also filtered is not an integer one whatever its loads feed (the PC
        // cannot sample those at all).
        std::set<int> sampled;
        for (auto& [i, r] : res_of)
            if (r >= 0 && ins[i].name && starts(ins[i].name, "image_sample")) sampled.insert(r);
        for (auto& [r, v] : vote)
            if ((v > 0 || (v == 0 && res[r].kind == ResKind::TypedBuf)) && !sampled.count(r)) res[r].is_uint = true;
    }

    const ResRecord* record(uint32_t param, uint32_t where, uint32_t offset_dw, bool any_offset = false) const {
        for (auto& r : meta.resources)
            if (r.param == param && r.where() == where && (any_offset || r.offset == offset_dw)) return &r;
        return nullptr;
    }
    // Shader register and space of the table slot `byte_off` of root parameter `param`.
    bool table_binding(uint32_t param, uint32_t byte_off, bool sampler, RangeType& type, uint32_t& reg, uint32_t& space) const {
        if (param >= rs.params.size()) return false;
        uint32_t slot = byte_off / (sampler ? 16 : 32);
        for (auto& r : rs.params[param].ranges) {
            if ((r.type == RangeType::Sampler) != sampler) continue;
            if (slot >= r.offset && (r.count == 0xffffffffu || slot < r.offset + r.count)) {
                type = r.type;
                reg = r.base + (slot - r.offset);
                space = r.space;
                return true;
            }
        }
        return false;
    }
    int add_res(Res r) {
        if (r.kind != ResKind::Counter && r.kind != ResKind::StaticSampler) refine(r);
        for (size_t i = 0; i < res.size(); i++)
            if (res[i].param == r.param && res[i].offset == r.offset && res[i].kind == r.kind && res[i].uav == r.uav) return int(i);
        r.name = fmt("R%zu", res.size());
        res.push_back(r);
        return int(res.size() - 1);
    }
    // Identifies the resource whose descriptor is in SGPRs [s, s+n).
    // With soft_trace, any descriptor that cannot be traced returns -1 without failing the
    // translation (the caller has a run-time path).
    int resource(const SState& st, uint16_t s, unsigned n, const Instr& in, bool sampler) {
        if (!soft_trace) return resource_traced(st, s, n, in, sampler);
        std::string saved = err;
        size_t nres = res.size();
        int r = resource_traced(st, s, n, in, sampler);
        if (err != saved) { err = saved; res.resize(nres); return -1; }
        return r;
    }
    int resource_traced(const SState& st, uint16_t s, unsigned n, const Instr& in, bool sampler) {
        AV w0 = st[s], w1 = st[s + 1];
        std::string nm = in.name;
        Res r{};
        if (w0.k == AV::Desc) {
            r.param = w0.a;
            r.offset = w0.b;
            RangeType type;
            const ResRecord* rec = record(w0.a, 3, w0.b / 4);
            if (w0.d && merged_reps.count(w0.d - 1) && merged_span.count(w0.d - 1) && !sampler) {
                // One of several table slots chosen by branch: bind just those slots as an array.
                auto [lo, hi] = merged_span[w0.d - 1];
                RangeType t0, t1;
                uint32_t reg1 = 0, space1 = 0;
                if (!table_binding(w0.a, lo, false, t0, r.reg, r.space) || !table_binding(w0.a, hi, false, t1, reg1, space1) || t0 != t1 ||
                    space1 != r.space || reg1 != r.reg + (hi - lo) / 32) {
                    fail(fmt("%s at 0x%x: descriptors chosen by branch span several root-signature ranges", in.name, in.offset));
                    return -1;
                }
                r.offset = 0xC0000000u | lo;
                r.array = (hi - lo) / 32 + 1;
                r.dyn = true;
                res_index[cur_idx] = fmt("((dyn%u + %uu) / 32u - %uu)", w0.d - 1, w0.b, lo / 32);
                dyn_loads.insert(w0.d - 1);
                r.uav = t0 == RangeType::UAV;
                const ResRecord* rec = record(w0.a, 3, lo / 4);
                if (in.fmt == Fmt::MTBUF || has(nm, "_format")) r.kind = ResKind::TypedBuf;
                else if (n == 8) r.kind = ResKind::Image;
                else if (!rec || rec->elem <= 1) r.kind = ResKind::RawBuf;
                else { r.kind = ResKind::StructBuf; r.stride = rec->elem; }
                return add_res(r);
            }
            if (w0.d) {
                // Run-time slot: bind the whole root-signature range as an array.
                rec = record(w0.a, 3, 0, true);
                for (auto& rr : meta.resources)
                    if (rr.param == w0.a && rr.where() == 3 && ((rr.cls() == 1) == sampler)) rec = &rr;
                uint32_t base_off = rec ? rec->offset * 4 : 0;
                if (!table_binding(w0.a, base_off, sampler, type, r.reg, r.space)) {
                    fail(fmt("%s at 0x%x: no root-signature range for table %u", in.name, in.offset, w0.a));
                    return -1;
                }
                const RsRange* range = nullptr;
                for (auto& rg : rs.params[w0.a].ranges)
                    if ((rg.type == RangeType::Sampler) == sampler && r.reg >= rg.base && (rg.count == 0xffffffffu || r.reg < rg.base + rg.count)) range = &rg;
                r.offset = 0x80000000u | (range ? range->offset : 0);
                r.array = range ? (range->count == 0xffffffffu ? 0u : range->count) : 1;
                r.reg = range ? range->base : r.reg;
                r.dyn = true;
                unsigned esz = sampler ? 16 : 32;
                res_index[cur_idx] = fmt("((dyn%u + %uu) / %uu - %uu)", w0.d - 1, w0.b, esz, range ? range->offset : 0);
                dyn_loads.insert(w0.d - 1);
            } else if (!table_binding(w0.a, w0.b, sampler, type, r.reg, r.space)) {
                fail(fmt("%s at 0x%x: no root-signature range for table %u offset %u", in.name, in.offset, w0.a, w0.b));
                return -1;
            }
            if (!sampler && !w0.d && type == RangeType::UAV && (w0.b % 32) == 16) {
                // The second half of a 32-byte UAV slot holds the descriptor of its hidden counter.
                Res buf{};
                buf.param = w0.a;
                buf.offset = w0.b - 16;
                buf.reg = r.reg;
                buf.space = r.space;
                buf.uav = true;
                const ResRecord* brec = record(w0.a, 3, (w0.b - 16) / 4);
                buf.kind = brec && brec->elem > 1 ? ResKind::StructBuf : ResKind::RawBuf;
                buf.stride = brec ? brec->elem : 0;
                r.kind = ResKind::Counter;
                r.counter_of = add_res(buf);
                return add_res(r);
            }
            if (sampler) {
                r.kind = ResKind::Sampler;
            } else if (type == RangeType::CBV) {
                r.kind = ResKind::CBuffer;
            } else {
                r.uav = type == RangeType::UAV;
                if (n == 8) r.kind = ResKind::Image;
                else if (in.fmt == Fmt::MTBUF || has(nm, "_format")) r.kind = ResKind::TypedBuf;
                else if (!rec || rec->elem == 1 || rec->elem == 0) r.kind = ResKind::RawBuf;
                else {
                    r.kind = ResKind::StructBuf;
                    r.stride = rec->elem;
                }
            }
            return add_res(r);
        }
        if (w0.k == AV::RootLo || (w0.k == AV::Arg && (w1.k == AV::RootHi || w1.k == AV::Arg))) {
            uint32_t loc = w0.k == AV::Arg ? w0.a : 0;
            uint32_t param = 0;
            uint32_t boff = w0.k == AV::Arg ? w0.b : w0.b;
            const UserArg* a = nullptr;
            if (w0.k == AV::Arg) a = arg_at(loc);
            if (!a || a->kind != 1) {
                fail(fmt("%s at 0x%x: descriptor is not a table entry or root descriptor", in.name, in.offset));
                return -1;
            }
            param = a->param;
            const RsParam& p = rs.params.at(param);
            r.param = param;
            r.offset = boff;
            r.reg = p.reg;
            r.space = p.space;
            r.root_hi_or = w1.k == AV::RootHi ? w1.c : 0;
            r.addr_off = boff;
            const ResRecord* rec = record(param, 1, 0, true);
            if (p.type == ParamType::CBV) r.kind = ResKind::CBuffer;
            else {
                r.uav = p.type == ParamType::UAV;
                if (!rec || rec->elem <= 1) r.kind = ResKind::RawBuf;
                else {
                    r.kind = ResKind::StructBuf;
                    r.stride = rec->elem;
                }
            }
            return add_res(r);
        }
        bool all_const = st[s].k == AV::Const && st[s + 1].k == AV::Const;
        if (sampler && all_const) {
            // Static sampler baked into the code: match by register when there is one candidate.
            const ResRecord* only = nullptr;
            int count = 0;
            for (auto& rr : meta.resources)
                if (rr.kind == 0x401) {
                    only = &rr;
                    count++;
                }
            // XSL_SAMPLER_PAIRS=1 (diagnostics): the constant sampler words and the static samplers
            // the shader's records name, to check the decoding of the words against the root signature.
            static const bool pairs = getenv("XSL_SAMPLER_PAIRS") != nullptr;
            if (pairs && count >= 1) {
                std::string t = fmt("SAMP %08x %08x %08x %08x n=%d", st[s].a, st[s + 1].a, st[s + 2].k == AV::Const ? st[s + 2].a : 0xdeadbeefu,
                                    st[s + 3].k == AV::Const ? st[s + 3].a : 0xdeadbeefu, count);
                for (auto& rr : meta.resources) {
                    if (rr.kind != 0x401) continue;
                    for (auto& ss : rs.samplers)
                        if (ss.reg() == rr.reg) {
                            t += fmt(" | reg %u:", rr.reg);
                            for (int k = 0; k < 10; k++) t += fmt(" %x", ss.words[k]);
                        }
                }
                fprintf(stderr, "%s\n", t.c_str());
            }
            if (count >= 1) {
                r.kind = ResKind::StaticSampler;
                r.reg = only->reg;
                r.param = 0xffff;
                r.offset = count == 1 ? only->reg : (st[s].a ^ st[s + 1].a * 31u ^ st[s + 2].a * 131u);
                for (auto& ss : rs.samplers)
                    if (ss.reg() == only->reg) r.space = ss.space();
                if (count > 1) {
                    // Several static samplers: the one whose state the four sampler words in the code
                    // hold. The words (checked against 8,000 shaders with one static sampler):
                    // word 0 = address modes of x, y, z (3 bits each from bit 0: 0 wrap, 1 mirror,
                    // 2 clamp, 3 mirror once, 6 border), log2 of the anisotropy (bits 9-11), comparison
                    // function less one (bits 12-14); word 1 = lowest and highest level in 1/256
                    // (12 bits each); word 2 = level bias (signed 14 bits, 1/256), magnification and
                    // minification filters (bits 20-21, 22-23: 1 linear, 2-3 anisotropic), mip filter
                    // (bits 26-27: 2 linear); word 3 bits 30-31 = border colour. A word 2 the code does
                    // not give as a constant goes with point filtering in every shader seen.
                    const uint32_t h0 = st[s].a, h1 = st[s + 1].a;
                    const uint32_t h2 = st[s + 2].k == AV::Const ? st[s + 2].a : 0, h3 = st[s + 3].k == AV::Const ? st[s + 3].a : 0;
                    auto addr = [](uint32_t hw) { return hw == 0 ? 1u : hw == 1 ? 2u : hw == 2 ? 3u : hw == 3 ? 5u : hw == 6 ? 4u : 0u; };
                    const uint32_t mag = (h2 >> 20) & 3, mn = (h2 >> 22) & 3, mip = (h2 >> 26) & 3, func = (h0 >> 12) & 7;
                    const bool aniso = mag >= 2 || mn >= 2;
                    const float hmin = float(h1 & 0xfff) / 256.0f, hmax = float((h1 >> 12) & 0xfff) / 256.0f;
                    const float hbias = float(int32_t(h2 << 18) >> 18) / 256.0f;
                    auto near_ = [](float x, float y) { float d = x - y; return d < 0.006f && d > -0.006f; };
                    auto lod = [](float v) { return v < 0 ? 0.0f : v > 4095.0f / 256.0f ? 4095.0f / 256.0f : v; };
                    const RsStaticSampler* best = nullptr;
                    bool differ = false;
                    for (auto& ss : rs.samplers) {
                        bool listed = false;
                        for (auto& rr : meta.resources)
                            if (rr.kind == 0x401 && rr.reg == ss.reg()) listed = true;
                        if (!listed) continue;
                        const uint32_t f = ss.words[0];
                        float bias, lo, hi;
                        memcpy(&bias, &ss.words[4], 4);
                        memcpy(&lo, &ss.words[8], 4);
                        memcpy(&hi, &ss.words[9], 4);
                        bool ok = addr(h0 & 7) == ss.words[1] && addr((h0 >> 3) & 7) == ss.words[2] && addr((h0 >> 6) & 7) == ss.words[3];
                        ok = ok && ((f & 0x40) != 0) == aniso;
                        if (aniso) ok = ok && (1u << ((h0 >> 9) & 7)) == ss.words[5];
                        else ok = ok && ((f >> 2) & 1) == (mag & 1) && ((f >> 4) & 1) == (mn & 1) && (f & 1) == (mip == 2 ? 1u : 0u);
                        ok = ok && ((f & 0x80) ? func + 1 == ss.words[6] : func == 0);
                        ok = ok && near_(hbias, bias) && near_(hmin, lod(lo)) && near_(hmax, lod(hi));
                        // (The border colour only when the code gives word 3: with a white border it comes at run time.)
                        if (st[s + 3].k == AV::Const && (ss.words[1] == 4 || ss.words[2] == 4 || ss.words[3] == 4)) ok = ok && (h3 >> 30) == ss.words[7];
                        if (!ok) continue;
                        if (best && memcmp(best->words, ss.words, 10 * sizeof(uint32_t))) differ = true;   // (equal ones are interchangeable)
                        if (!best) best = &ss;
                    }
                    if (!best || differ) {
                        fail(fmt("%s at 0x%x: several static samplers and %s matches the sampler words %08x %08x %08x %08x", in.name, in.offset,
                                 best ? "more than one" : "none", h0, h1, h2, h3));
                        return -1;
                    }
                    r.reg = best->reg();
                    r.space = best->space();
                    r.offset = best->reg();
                }
                return add_res(r);
            }
        }
        if (soft_trace) return -1;   // the caller has a run-time path (descriptor words read from SGPRs)
        fail(fmt("%s at 0x%x: cannot trace descriptor s[%u:%u] (kinds %d %d)", in.name, in.offset, s, s + n - 1, int(w0.k), int(w1.k)));
        return -1;
    }
    void resolve(size_t i, const SState& st) {
        const Instr& in = ins[i];
        if (!in.name) return;
        // XSL_DBG_AT=<hex offset>: print the scalar analysis state at that instruction (debugging).
        static const long dbg_at = getenv("XSL_DBG_AT") ? (long)strtoul(getenv("XSL_DBG_AT"), nullptr, 16) : -1;
        if (dbg_at >= 0 && (long)in.offset == dbg_at)
            for (int r = 0; r < 24; r++) fprintf(stderr, "s%d: k%d a%u b%u c%u d%u\n", r, int(st[r].k), st[r].a, st[r].b, st[r].c, st[r].d);
        std::string n = in.name;
        if ((in.fmt == Fmt::MUBUF) && (n == "buffer_gl0_inv" || n == "buffer_gl1_inv")) {
            // cache invalidations: nothing to do on PC (no descriptor)
        } else if (in.fmt == Fmt::MUBUF || in.fmt == Fmt::MTBUF) {
            // Raw dword loads/stores may take a descriptor that only exists at run time (loaded from
            // memory): with global memory they address memory from the descriptor words directly.
            bool rawDw = ((in.fmt == Fmt::MUBUF && (starts(n, "buffer_load_dword") || starts(n, "buffer_store_dword") || starts(n, "buffer_load_format_") ||
                                                    starts(n, "buffer_store_format_") || dyn_atomic_op(n) || dyn_small_op(n))) ||
                          (in.fmt == Fmt::MTBUF && starts(n, "tbuffer_load_format_"))) && !in.lds;
            soft_trace = rawDw && (opt.global_memory || opt.global_memory_test);
            int r = resource(st, in.srsrc, 4, in, false);
            soft_trace = false;
            if (r < 0 && rawDw && (opt.global_memory || opt.global_memory_test)) dyn_mubuf.insert(i);
            res_of[i] = r;
            if (r >= 0 && has(n, "_atomic_") && res[r].kind == ResKind::TypedBuf) { res[r].scalar = true; res[r].is_uint = true; }
        }
        else if (in.fmt == Fmt::MIMG) {
            bool bvh = has(n, "bvh");   // inert acceleration structures (emit_mimg): no resource
            // Image loads/stores may take a descriptor read from descriptor memory at run time: the
            // layer stamps its global-heap index and view kind into descriptor dwords 0 and 1.
            bool dynSample = starts(n, "image_sample") || starts(n, "image_gather4") || n == "image_get_lod" || n == "image_get_resinfo";
            bool dynOk = !bvh && (n == "image_load" || n == "image_store" || n == "image_load_mip" || dynSample) && (opt.global_memory || opt.global_memory_test);
            soft_trace = dynOk;
            int r = bvh ? -2 : resource(st, in.srsrc, 8, in, false);
            soft_trace = false;
            if (r < 0 && dynOk) dyn_mimg.insert(i);
            res_of[i] = r;
            if (r >= 0 && starts(n, "image_atomic")) { res[r].is_uint = true; res[r].scalar = true; }
            if (starts(n, "image_sample") || starts(n, "image_gather4") || n == "image_get_lod") {
                std::string keep = res_index.count(i) ? res_index[i] : "";
                res_index.erase(i);
                soft_trace = dynOk;
                samp_of[i] = resource(st, in.ssamp, 4, in, true);
                soft_trace = false;
                if (samp_of[i] < 0 && dynOk) dyn_samp.insert(i);
                if (res_index.count(i)) samp_index[i] = res_index[i];
                res_index.erase(i);
                if (!keep.empty()) res_index[i] = keep;
            }
        } else if (in.fmt == Fmt::SMEM && starts(n, "s_buffer_load")) {
            // A descriptor known only at run time: its base address through global memory.
            soft_trace = opt.global_memory || opt.global_memory_test;
            res_of[i] = resource(st, in.sbase, 4, in, false);
            soft_trace = false;
            if (res_of[i] < 0 && (opt.global_memory || opt.global_memory_test)) dyn_sbuf.insert(i);
        } else if (in.fmt == Fmt::SMEM && starts(n, "s_load")) {
            // Descriptor loads: values come from synthesized descriptor words when read as data.
            unsigned cnt = has(n, "x16") ? 16 : has(n, "x8") ? 8 : has(n, "x4") ? 4 : has(n, "x2") ? 2 : 1;
            AV base = st[in.sbase];
            std::vector<std::string> v;
            for (unsigned k = 0; k < cnt; k++) v.push_back("0u");
            // A pointer computed at run time (not a table, root argument or code address): the
            // dwords come from memory through the layer's address table.
            if ((base.k == AV::Top || base.k == AV::Undef) && (opt.global_memory || opt.global_memory_test)) gm_sloads.insert(i);
            if (cnt == 8) {   // an image descriptor: its size fields come from the bound texture
                SState t = st;
                step(t, in);
                if (t[in.sdata].k == AV::Desc) {
                    int r = resource(t, in.sdata, 8, in, false);
                    if (r >= 0 && res[r].kind == ResKind::Image && !res[r].dyn) {
                        res[r].need_desc = true;
                        for (unsigned k = 1; k <= 4; k++) v[k] = fmt("desc_%s()[%u]", res[r].name.c_str(), k - 1);
                    }
                }
            }
            if (base.k == AV::Pc) {
                AV so = in.soffset == src::kNull ? AV::cst(0) : sval_of(st, in.soffset);
                for (unsigned k = 0; k < cnt; k++) v[k] = fmt("0x%xu", code_dword(base.b + in.offset_imm + so.a + 4 * k));
            }
            if (base.k == AV::Arg && base.a == kMemBlock) {
                // Root constants in the memory-based argument block come from their constant buffer.
                AV so = in.soffset == src::kNull ? AV::cst(0) : sval_of(st, in.soffset);
                for (unsigned k = 0; k < cnt; k++) {
                    uint32_t dw = (uint32_t(in.offset_imm) + so.a + base.b) / 4 + k;
                    for (auto& a : meta.args)
                        if (a.kind == 0 && a.loc >= 32 && dw >= a.loc - 32 && dw < a.loc - 32 + a.size && a.param < rs.params.size()) {
                            uint32_t j = dw - (a.loc - 32);
                            v[k] = fmt("RC%u_d[%u][%u]", a.param, j / 4, j % 4);
                        }
                }
            }
            sval[i] = v;
        }
    }

    // ---- HLSL emission helpers ----------------------------------------------------------------
    std::string S(unsigned r) const {
        if (r == 106) return "s106";
        if (r == 107) return "s107";
        if (r == src::kM0) return "s124";
        if (r == src::kExecLo) return "s126";
        if (r == src::kExecHi) return "s127";
        if (r == src::kNull) return "snull";
        if (r < kNumS) return "s" + std::to_string(r);
        return "0u";
    }
    std::string V(unsigned r) {
        max_v = std::max(max_v, r + 1);
        if (dual) return fmt("v%u_%d", r, cp < 0 ? 0 : cp);
        return "v" + std::to_string(r);
    }
    std::string Vc(unsigned r, int c) { max_v = std::max(max_v, r + 1); return fmt("v%u_%d", r, c); }
    std::string mask_bool(unsigned r) const {   // this lane's bit of the mask held in SGPR r
        if (r == src::kExecLo) return "kexec";
        if (r == 106) return "kvcc";
        if (W == 64) return fmt("lanebit(%s, %s)", S(r).c_str(), S(r + 1).c_str());
        return fmt("lanebit1(%s)", S(r).c_str());
    }
    // Inline-constant value for operand type t ('f' f32, 'h' f16, 'd' f64 high word, else integer).
    std::string inl(uint16_t e, char t, const Instr& in) const {
        if (e >= src::kZero && e <= src::kIntPos64) return fmt("%uu", e - src::kZero);
        if (e >= src::kIntNeg1 && e <= src::kIntNeg16) return fmt("0x%xu", uint32_t(-(int(e) - 192)));
        static const float fv[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f, 0.15915494f};
        static const uint32_t hv[] = {0x3800, 0xb800, 0x3c00, 0xbc00, 0x4000, 0xc000, 0x4400, 0xc400, 0x3118};
        static const uint32_t dv[] = {0x3fe00000, 0xbfe00000, 0x3ff00000, 0xbff00000, 0x40000000, 0xc0000000, 0x40100000, 0xc0100000, 0x3fc45f30};
        if (e >= src::kFloatHalf && e <= src::kInvTwoPi) {
            int k = e - src::kFloatHalf;
            if (t == 'h') return fmt("0x%xu", hv[k]);
            if (t == 'd') return fmt("0x%xu", dv[k]);
            uint32_t u;
            memcpy(&u, &fv[k], 4);
            return fmt("0x%xu", u);
        }
        if (e == src::kLiteral) return fmt("0x%xu", in.literal);
        if (e == src::kScc) return "(scc ? 1u : 0u)";
        // wave32 code: only the low register is the mask (the high one may hold anything)
        if (e == src::kVccz) return W == 64 ? "((s106 | s107) == 0u ? 1u : 0u)" : "(s106 == 0u ? 1u : 0u)";
        if (e == src::kExecz) return W == 64 ? "((s126 | s127) == 0u ? 1u : 0u)" : "(s126 == 0u ? 1u : 0u)";
        // Primitive-ordered pixel shading: code waits in a loop until the wave id read here has
        // passed its own (overlapping pixels shade in submission order). A PC has no such
        // register; the ordering is given up and the wait is made to end: successive reads step
        // through the id space in quarters, one of which every "has it passed" compare accepts.
        if (e == src::kPopsExitingWaveId) return "pops_next()";
        return "";
    }
    // 32-bit source bits.
    std::string rd(uint16_t e, const Instr& in, char t = 'u') {
        if (e >= src::kVgpr0) return V(e - src::kVgpr0);
        if (e < kNumS) return S(e);
        std::string k = inl(e, t, in);
        if (k.empty()) {
            fail(fmt("%s at 0x%x: unsupported operand %u", in.name, in.offset, e));
            return "0u";
        }
        return k;
    }
    std::string rd_hi(uint16_t e, const Instr& in, char t = 'u') {   // high dword of a 64-bit operand
        if (e >= src::kVgpr0) return V(e - src::kVgpr0 + 1);
        if (e < kNumS) return S(e + 1);
        if (e == src::kLiteral) return "0u";
        if (e >= src::kIntNeg1 && e <= src::kIntNeg16) return "0xffffffffu";
        if (t == 'd' && e >= src::kFloatHalf && e <= src::kInvTwoPi) return inl(e, 'd', in);
        return "0u";
    }
    std::string rd64(uint16_t e, const Instr& in, char t = 'u') {
        if (t == 'd' && e >= src::kFloatHalf && e <= src::kInvTwoPi)
            return fmt("(uint64_t(%s) << 32)", inl(e, 'd', in).c_str());
        if (t == 'd' && e == src::kLiteral) return fmt("(uint64_t(0x%xu) << 32)", in.literal);
        return fmt("(uint64_t(%s) | (uint64_t(%s) << 32))", rd(e, in).c_str(), rd_hi(e, in).c_str());
    }

    void line(const std::string& s) { out << "  " << s << "\n"; }
    // v_movrel*: "@@MOVREL{mode|dst|src|suffix|value}@@" -> switches over VGPRs 0..nv-1 (index = M0).
    static std::string expand_movrel(const std::string& b, unsigned nv) {
        std::string r;
        size_t p = 0;
        for (size_t q; (q = b.find("@@MOVREL{", p)) != std::string::npos;) {
            size_t e = b.find("}@@", q);
            r += b.substr(p, q - p);
            std::vector<std::string> f;
            std::string body = b.substr(q + 9, e - q - 9);
            for (size_t a = 0, c; ; a = c + 1) { c = body.find('|', a); f.push_back(body.substr(a, c == std::string::npos ? std::string::npos : c - a)); if (c == std::string::npos || f.size() == 4) { if (c != std::string::npos) f.push_back(body.substr(c + 1)); break; } }
            char mode = f[0][0];
            unsigned d = (unsigned)std::stoul(f[1]), so = (unsigned)std::stoul(f[2]);
            const std::string& sf = f[3];
            std::string val = f.size() > 4 ? f[4] : "";
            std::string out = "if (kexec" + sf + ") { uint mr_ = 0u; ";   // (sf: the lane copy, as in the registers' names)
            if (mode != 'd') {   // read VGPR[src + M0]
                out += "switch (s124 + " + std::to_string(so) + "u) { ";
                for (unsigned k = 0; k < nv; k++) out += "case " + std::to_string(k) + "u: mr_ = v" + std::to_string(k) + sf + "; break; ";
                out += "default: break; } ";
            } else out += "mr_ = " + val + "; ";
            if (mode == 's') out += "v" + std::to_string(d) + sf + " = mr_; }";
            else {   // write VGPR[dst + M0]
                out += "switch (s124 + " + std::to_string(d) + "u) { ";
                for (unsigned k = 0; k < nv; k++) out += "case " + std::to_string(k) + "u: v" + std::to_string(k) + sf + " = mr_; break; ";
                out += "default: break; } }";
            }
            r += out;
            p = e + 3;
        }
        return r + b.substr(p);
    }
    void write_s(unsigned r, const std::string& v) {
        if (r == src::kNull || r >= kNumS) return;
        line(S(r) + " = " + v + ";");
        sync_mask_from_int(r);
    }
    void sync_mask_from_int(unsigned r) {
        if (dual) {   // masks are exact wave64 integers: derive both copies' bits
            if (r == src::kExecLo || r == src::kExecHi) line("kexec_0 = lanebitL(s126, s127, LP_); kexec_1 = lanebitL(s126, s127, LP_ + 32u);");
            else if (r == 106 || r == 107) line("kvcc_0 = lanebitL(s106, s107, LP_); kvcc_1 = lanebitL(s106, s107, LP_ + 32u);");
            return;
        }
        if (r == src::kExecLo || r == src::kExecHi) line(W == 64 ? "kexec = lanebit(s126, s127);" : "kexec = lanebit1(s126);");
        else if (r == 106 || r == 107) line(W == 64 ? "kvcc = lanebit(s106, s107);" : "kvcc = lanebit1(s106);");
    }
    void write_mask(unsigned r, const std::string& b) {   // r: mask register (pair base); b: this lane's bit
        if (r == src::kNull) return;
        if (dual) {   // this copy's half of the wave64 mask
            const char* half = cp == 1 ? "hi" : "lo";
            unsigned reg = r == src::kExecLo ? 126 : r;
            if (r == src::kExecLo) line("kexec = " + b + ";");
            else if (r == 106) line("kvcc = " + b + ";");
            if (reg < kNumS) line(fmt("%s = ballot2(%s).x;   // %s half", S(reg + (cp == 1 ? 1 : 0)).c_str(), r == src::kExecLo ? "kexec" : r == 106 ? "kvcc" : ("(" + b + ")").c_str(), half));
            return;
        }
        if (r == src::kExecLo) {
            line("kexec = " + b + ";");
            line(W == 64 ? "{ uint2 b_ = ballot2(kexec); s126 = b_.x; s127 = b_.y; }" : "s126 = ballot2(kexec).x;");
        } else if (r == 106) {
            line("kvcc = " + b + ";");
            line(W == 64 ? "{ uint2 b_ = ballot2(kvcc); s106 = b_.x; s107 = b_.y; }" : "s106 = ballot2(kvcc).x;");
        } else if (r < 106) {
            line(fmt("{ bool m_ = %s; uint2 b_ = ballot2(m_); %s = b_.x;%s }", b.c_str(), S(r).c_str(),
                     W == 64 ? (" " + S(r + 1) + " = b_.y;").c_str() : ""));
        }
    }
    std::string lane_pred;   // extra per-lane write condition of the current instruction (DPP)
    void write_v(unsigned r, const std::string& v, bool pred = true) {
        if (pred && !lane_pred.empty()) {
            line("{ uint x_ = " + v + "; if (kexec && (" + lane_pred + ")) " + V(r) + " = x_; }");
            return;
        }
        // Cross-lane reads are evaluated by every lane (outside the EXEC predicate), so the lanes
        // they read from are active on the PC as well.
        if (pred && (v.find("xlane") != std::string::npos || v.find("WaveReadLaneAt") != std::string::npos || v.find("quadperm") != std::string::npos))
            line("{ uint x_ = " + v + "; if (kexec) " + V(r) + " = x_; }");
        else
            line((pred ? "if (kexec) " : "") + V(r) + " = " + v + ";");
    }

    // ---- instruction emission ---------------------------------------------------------------
    bool emit_salu(const Instr& in, size_t idx);
    bool emit_valu(const Instr& in, size_t idx);
    bool emit_smem(const Instr& in, size_t idx);
    bool emit_mubuf(const Instr& in, size_t idx);
    bool emit_tbuffer_raw(const Instr& in, const Res& R, const std::string& RN, const std::string& addr,
                          const std::function<std::string(const std::string&)>& word);
    bool emit_mimg(const Instr& in, size_t idx);
    bool emit_heap_image(const Instr& in, size_t idx);
    bool emit_ds(const Instr& in, size_t idx);
    bool emit_vintrp(const Instr& in, size_t idx);
    bool emit_exp(const Instr& in, size_t idx);
    bool emit_global(const Instr& in, size_t idx);
    std::string attr_value(unsigned attr, unsigned chan, const std::string& mode);
    std::string attr_at(unsigned reg, unsigned chan, const std::string& mode, const SigElem** elem = nullptr);
    bool emit_interp16(const Instr& in);
    bool emit_param_records(const Instr& in, unsigned p, const std::string comp[4]);
    int half_partner(unsigned row, bool pixel = false);
    bool per_lane(const Instr& in) const {
        switch (in.fmt) {
        case Fmt::VOP1: case Fmt::VOP2: case Fmt::VOPC: case Fmt::VOP3: case Fmt::VOP3P:
            return strcmp(in.name, "v_readlane_b32") && strcmp(in.name, "v_readfirstlane_b32");
        case Fmt::VINTRP: case Fmt::DS: case Fmt::MUBUF: case Fmt::MTBUF: case Fmt::MIMG: case Fmt::GLOBAL: case Fmt::FLAT:
        case Fmt::SCRATCH: case Fmt::EXP:
            return true;
        default:
            return false;
        }
    }
    uint64_t addr_of(uint32_t offset) const {
        for (auto& sg : opt.raw_segments)
            if (offset >= sg.offset && offset < sg.offset + sg.size) return sg.address + (offset - sg.offset);
        return opt.code_base + offset;
    }
    // dst[k] = dword k at (lo_, hi_) through global memory: one search for all of them (dwords past
    // the found range read 0).
    void gm_read_n(const std::vector<std::string>& dst) {
        if (dst.size() == 1) { line(fmt("  %s = gm_load(lo_, hi_);", dst[0].c_str())); return; }
        line("  { uint fy_, fz_, fl_; bool fx_ = gm_find(lo_, hi_, fy_, fz_, fl_);");
        if (opt.raw_stats) line("    dbg_add(21u); if (!fx_) { dbg_add(12u); dbg_set2(13u, lo_, hi_); }");
        for (size_t k = 0; k < dst.size(); k++)
            line(fmt("    %s = fx_ && fl_ > %zuu ? gm_vload(fy_, fz_ + %zuu) : 0u;", dst[k].c_str(), 4 * k, 4 * k));
        line("  }");
    }
    std::string park_lane() const {
        return "((((gid_.z * RC0_d[0].w + gid_.y) * RC0_d[0].z + gid_.x) * WAVE) + LG)";
    }
    void emit_park() {
        const unsigned nv = opt.raw_park_vgprs, stride = 8 + 4 * nv;
        line("    uint p_ = (tlo_ >> 8) | (thi_ << 24);");
        line(fmt("    if (kexec && %s < RC0_d[1].w) { uint b_ = %s * %uu;", park_lane().c_str(), park_lane().c_str(), stride));
        line("      RTST.Store2(b_, uint2(p_, RC0_d[1].z + 1u));");
        for (unsigned k = 0; k < nv; k += 4) line(fmt("      RTST.Store4(b_ + %uu, uint4(v%u, v%u, v%u, v%u));", 8 + 4 * k, k, k + 1, k + 2, k + 3));
        line(fmt("      uint u_ = (p_ - 0x%xu) >> %uu; if (u_ < (1u << 20)) RTDISC.InterlockedOr((u_ >> 5) * 4u, 1u << (u_ & 31u)); }", uint32_t(opt.code_base >> 8),
                 opt.raw_park_disc_shift));
        if (opt.raw_stats) line("    if (kexec) { dbg_add(6u); dbg_set2(7u, tlo_, thi_); }");
        line(fmt("    if (kexec) v%u = 0u;", opt.raw_park_ptr_vgpr));
        max_v = std::max(max_v, nv);
        uses_park = true;
    }
    void emit_resume() {
        const unsigned nv = opt.raw_park_vgprs, stride = 8 + 4 * nv;
        line("  if ((RC0_d[1].z & 0xffu) != 0u) {   // resume lanes parked in the previous round on code of this program");
        line(fmt("    uint l_ = %s, p_ = 0u; bool r_ = false;", park_lane().c_str()));
        line(fmt("    if (l_ < RC0_d[1].w) { uint2 h_ = RTST.Load2(l_ * %uu); p_ = h_.x; r_ = h_.y == RC0_d[1].z && jump_case(p_ << 8, p_ >> 24) != 0xffffffffu; }", stride));
        line(fmt("    if (r_) { uint b_ = l_ * %uu; uint4 q_;", stride));
        for (unsigned k = 0; k < nv; k += 4)
            line(fmt("      q_ = RTST.Load4(b_ + %uu); v%u = q_.x; v%u = q_.y; v%u = q_.z; v%u = q_.w;", 8 + 4 * k, k, k + 1, k + 2, k + 3));
        line(fmt("      v%u = p_; } else v%u = 0u;", opt.raw_park_ptr_vgpr, opt.raw_park_ptr_vgpr));
        if (opt.raw_stats) line("    if (r_) dbg_add(9u);");
        line("  }");
        if (opt.raw_stats) {   // at the scheduler's "pointer > 0" test, every round
            line(fmt("  if (kexec) { dbg_add(24u); if (int(v%u) > 0) dbg_add(23u); else dbg_set2(25u, v%u, RC0_d[1].z); dbg_add(56u + min(RC0_d[1].z & 0xffu, 7u)); }",
                     opt.raw_park_ptr_vgpr, opt.raw_park_ptr_vgpr));
            // the scheduler's table values (s12..s27), group ids and launch coordinates of a lane without a pointer in round 0
            line(fmt("  if (kexec && int(v%u) <= 0 && (RC0_d[1].z & 0xffu) == 0u) { RTDBG.Store4(DBGB + 256u, uint4(s12, s13, s14, s15)); RTDBG.Store4(DBGB + 272u, uint4(s16, s17, s18, s19)); "
                     "RTDBG.Store4(DBGB + 288u, uint4(s20, s21, s22, s23)); RTDBG.Store4(DBGB + 304u, uint4(s24, s4, s5, s6)); RTDBG.Store4(DBGB + 320u, uint4(v0, v1, v2, v3)); "
                     "RTDBG.Store4(DBGB + 336u, uint4(s0, s1, s2, s28)); "
                     "{ uint ky_, kz_, kl_; bool kx_ = gm_find(s21 << 6, s21 >> 26, ky_, kz_, kl_); RTDBG.Store4(DBGB + 352u, uint4(kx_ ? 1u : 0u, ky_, kz_, kl_)); "
                     "RTDBG.Store4(DBGB + 368u, uint4(gm_vload(ky_, kz_), GMT[0].x, GMT[0].y, v%u)); } }", opt.raw_park_ptr_vgpr, opt.raw_park_ptr_vgpr));
        }
        if (opt.raw_trav_vgpr != ~0u)   // every lane, active or not: the scheduler reads lanes 0..2 of the wave
            line(fmt("  v%u = LG == 0u ? 0x%xu : LG == 1u ? 0x%xu : 0x%xu;", opt.raw_trav_vgpr, opt.raw_trav[0], opt.raw_trav[1], opt.raw_trav[2]));
        line("  {");
        // the compare is redone: the lane flag and the wave's VCC mask (the scheduler's saveexec and its
        // later exec arithmetic use the mask; a stale one re-activated lanes that were not resumed)
        line(fmt("    kvcc = kexec && int(v%u) > 0; s106 = ballot2(kvcc).x; s107 = 0u;", opt.raw_park_ptr_vgpr));
        line("  }");
        max_v = std::max(max_v, nv);
        uses_park = true;
    }
    bool emit(const Instr& in, size_t idx) {
        // Section markers in linked code: v_nop in its VOP3 encoding (opcode 384, absent from the
        // generated table) and v_illegal; they are never reached by execution.
        if (!in.name && in.fmt == Fmt::VOP3 && in.op == 384) return true;
        if (!in.name) return fail(fmt("unknown %s opcode %u at 0x%x", fmt_name(in.fmt), in.op, in.offset));
        if (!strcmp(in.name, "v_illegal")) return true;
        if (dual && per_lane(in)) {
            // Once per console lane copy: L/LG = console lane, EXEC/VCC bits and captured outputs
            // renamed. The names are replaced in the copy's text (they were preprocessor macros
            // switched around every instruction: 34 lines for one multiply, a 141 KB program came
            // to 14 MB of HLSL), and the lane is set only for a copy that reads it.
            bool ok = true;
            for (int c = 0; c < 2 && ok; c++) {
                cp = c;
                std::ostringstream saved;
                std::swap(saved, out);
                ok = emit1(in, idx);
                bool usesLane = false;
                const std::string t = lane_names(out.str(), c, &usesLane);
                std::swap(saved, out);
                if (usesLane) out << fmt("  L = LP_ + %du; LG = L;\n", 32 * c);
                lane_stale = !usesLane;   // (after copy 1: whether L is what every instruction used to leave, the second lane)
                out << t;
            }
            cp = -1;
            return ok;
        }
        cp = 0;
        if (dual && lane_stale) {
            // An instruction emitted once: should it read the lane, it reads the second copy's, as
            // it did when every per-lane instruction set it.
            std::ostringstream saved;
            std::swap(saved, out);
            bool ok = emit1(in, idx);
            bool usesLane = false;
            const std::string t = lane_names(out.str(), -1, &usesLane);
            std::swap(saved, out);
            if (usesLane) { out << "  L = LP_ + 32u; LG = L;\n"; lane_stale = false; }
            out << t;
            cp = -1;
            return ok;
        }
        bool ok = emit1(in, idx);
        cp = -1;
        return ok;
    }
    bool lane_stale = false;   // two-lane mode: L may not be the second lane's (its last users did not read it)
    int open_block = -1;       // straight-line code with barriers: the block whose "if (reach[b]) {" is open (else -1)
    // Loops and barriers (compute): the threads of a group take turns. Each runs until its next
    // barrier or its end; then all of them meet (every thread makes the same calls, whatever its
    // path, which is what a PC barrier needs), and the rounds go on until none has code left.
    // On the console a wave at a barrier waits for the waves still running, and one that ends
    // early or branches around the barrier keeps nobody waiting; a PC thread that does either
    // leaves the rest of its group at the barrier for good, and the GPU is reset.
    bool coop = false;
    // The text of one lane copy with the per-lane names made that copy's (c < 0: unchanged), and
    // whether it reads the lane number.
    static std::string lane_names(const std::string& t, int c, bool* usesLane) {
        static const char* names[] = {"kexec", "kvcc", "O", "P", "prim_", "hasv_", "hasp_", "clipcull_"};
        std::string r;
        r.reserve(t.size() + 64);
        for (size_t i = 0; i < t.size();) {
            const unsigned char ch = (unsigned char)t[i];
            if (isalpha(ch) || ch == '_') {
                size_t j = i + 1;
                while (j < t.size() && (isalnum((unsigned char)t[j]) || t[j] == '_')) j++;
                const size_t n = j - i;
                r.append(t, i, n);
                // (L and LG are globals: the helpers that read them count too, which are those with
                // "lane" in their names and quadperm)
                if ((n == 1 && t[i] == 'L') || (n == 2 && t[i] == 'L' && t[i + 1] == 'G') || t.compare(i, n, "quadperm") == 0 ||
                    std::string_view(t).substr(i, n).find("lane") != std::string_view::npos)
                    *usesLane = true;
                else if (c >= 0)
                    for (auto nm : names)
                        if (strlen(nm) == n && !t.compare(i, n, nm)) { r += c ? "_1" : "_0"; break; }
                i = j;
            } else if (isdigit(ch)) {   // a number (0x1fu, 1.0e5): not an identifier
                size_t j = i + 1;
                while (j < t.size() && (isalnum((unsigned char)t[j]) || t[j] == '.' || t[j] == '_')) j++;
                r.append(t, i, j - i);
                i = j;
            } else r += t[i++];
        }
        return r;
    }
    bool emit1(const Instr& in, size_t idx) {
        lane_pred.clear();
        if (opt.raw_park && in.offset == opt.raw_resume_at && cp <= 0) emit_resume();
        if (opt.comments && cp <= 0) out << "  // " << fmt("%04x", in.offset) << ": " << disasm(in) << "\n";
        switch (in.fmt) {
        case Fmt::SOP1: case Fmt::SOP2: case Fmt::SOPC: case Fmt::SOPK: case Fmt::SOPP: return emit_salu(in, idx);
        case Fmt::VOP1: case Fmt::VOP2: case Fmt::VOPC: case Fmt::VOP3: case Fmt::VOP3P: return emit_valu(in, idx);
        case Fmt::SMEM: return emit_smem(in, idx);
        case Fmt::MUBUF: case Fmt::MTBUF: return emit_mubuf(in, idx);
        case Fmt::MIMG: return emit_mimg(in, idx);
        case Fmt::DS: return emit_ds(in, idx);
        case Fmt::VINTRP: return emit_vintrp(in, idx);
        case Fmt::EXP: return emit_exp(in, idx);
        case Fmt::GLOBAL: return emit_global(in, idx);
        default: return fail(fmt("%s at 0x%x: format %s not supported", in.name, in.offset, fmt_name(in.fmt)));
        }
    }

    std::string res_decl(const Res& r) const;
    // Vertex-output system values written through position exports.
    std::string sysv_outputs(const std::vector<SigElem>& v) const {
        std::string o;
        unsigned clip = 0, cull = 0;
        for (auto& e : v) {
            if (e.sysval == 4) o += "  uint rtai_ : SV_RenderTargetArrayIndex;\n";
            if (e.sysval == 5) o += "  uint vpi_ : SV_ViewportArrayIndex;\n";
            if (e.sysval == 2) clip += __builtin_popcount(e.mask);
            if (e.sysval == 3) cull += __builtin_popcount(e.mask);
        }
        if (clip) o += fmt("  float%s clip_ : SV_ClipDistance;\n", clip > 1 ? std::to_string(std::min(clip, 4u)).c_str() : "");
        if (cull) o += fmt("  float%s cull_ : SV_CullDistance;\n", cull > 1 ? std::to_string(std::min(cull, 4u)).c_str() : "");
        return o;
    }
    std::string clipcull_copy() const {   // after the program: distances into the output struct
        unsigned clip = 0, cull = 0;
        for (auto& e : osg) {
            if (e.sysval == 2) clip += __builtin_popcount(e.mask);
            if (e.sysval == 3) cull += __builtin_popcount(e.mask);
        }
        std::string o;
        for (unsigned i = 0; i < std::min(clip, 4u); i++) o += clip > 1 ? fmt("O.clip_[%u] = clipcull_[%u]; ", i, i) : fmt("O.clip_ = clipcull_[%u]; ", i);
        for (unsigned i = 0; i < std::min(cull, 4u); i++) o += cull > 1 ? fmt("O.cull_[%u] = clipcull_[%u]; ", i, clip + i) : fmt("O.cull_ = clipcull_[%u]; ", clip + i);
        return o;
    }
    void link_arrays() {
        for (auto& r : res) {
            if (r.dyn || r.kind == ResKind::Counter || r.kind == ResKind::StaticSampler || r.param >= rs.params.size()) continue;
            for (size_t k = 0; k < res.size(); k++) {
                const Res& d = res[k];
                if (!d.dyn || d.param != r.param || d.kind != r.kind || d.reg > r.reg || (d.array && r.reg >= d.reg + d.array) || d.space != r.space) continue;
                r.alias = int(k);
                r.alias_index = r.reg - d.reg;
                if (r.uav) res[k].uav = true;
            }
        }
    }
    std::string generate();
};

// ---- SALU ---------------------------------------------------------------------------------
bool Translator::emit_salu(const Instr& in, size_t idx) {
    std::string n = in.name;
    auto a = [&] { return rd(in.src0, in); };
    auto b = [&] { return rd(in.src1, in); };
    auto a64 = [&] { return rd64(in.src0, in); };
    auto b64 = [&] { return rd64(in.src1, in); };
    auto w64 = [&](const std::string& v) {
        line(fmt("{ uint64_t r_ = %s; %s = uint(r_); %s = uint(r_ >> 32); }", v.c_str(), S(in.dst).c_str(), S(in.dst + 1).c_str()));
        sync_mask_from_int(in.dst);
    };
    // Mask bit of a scalar operand (for logic ops on lane masks).
    auto bit = [&](uint16_t e) -> std::string {
        if (e == src::kExecLo) return "kexec";
        if (e == 106) return "kvcc";
        if (e < kNumS) return mask_bool(e);
        if (e == src::kIntNeg1) return "true";
        if (e == src::kZero) return "false";
        std::string k = rd(e, in);
        return W == 64 ? fmt("lanebit(%s, %s)", k.c_str(), rd_hi(e, in).c_str()) : fmt("lanebit1(%s)", k.c_str());
    };
    auto logic_mask = [&](unsigned d, const std::string& iv_lo, const std::string& iv_hi, const std::string& bv, bool set_scc = true) {
        const char* sc = set_scc ? " scc = (lo_ | hi_) != 0u;" : "";
        if (dual) {
            line(fmt("{ uint lo_ = %s, hi_ = %s;%s %s = lo_; %s = hi_; }", iv_lo.c_str(), iv_hi.c_str(), sc, S(d).c_str(), S(d + 1).c_str()));
            sync_mask_from_int(d);
            return;
        }
        // Integer view and lane bit of a mask result; SCC = result != 0. A 64-bit instruction in
        // wave32 code writes both registers (it may as well be integer arithmetic on a pair).
        if (W == 64 || ends(n, "_b64")) line(fmt("{ uint lo_ = %s, hi_ = %s; bool k_ = %s;%s %s = lo_; %s = hi_;%s }",
                              iv_lo.c_str(), iv_hi.c_str(), bv.c_str(), sc, S(d).c_str(), S(d + 1).c_str(),
                              d == src::kExecLo ? " kexec = k_;" : d == 106 ? " kvcc = k_;" : ""));
        else line(fmt("{ uint lo_ = %s; bool k_ = %s;%s %s = lo_;%s }", iv_lo.c_str(), bv.c_str(), set_scc ? " scc = lo_ != 0u;" : "", S(d).c_str(),
                      d == src::kExecLo ? " kexec = k_;" : d == 106 ? " kvcc = k_;" : ""));
    };
    if (in.fmt == Fmt::SOPP) {
        if (n == "s_endpgm" || starts(n, "s_branch") || starts(n, "s_cbranch")) return true;   // control flow: emitted by caller
        static const char* nops[] = {"s_nop", "s_waitcnt", "s_inst_prefetch", "s_clause", "s_setprio", "s_sleep",
                                     "s_ttracedata", "s_ttracedata_imm", "s_code_end", "s_round_mode", "s_denorm_mode", "s_icache_inv",
                                     "s_waitcnt_depctr", "s_incperflevel", "s_decperflevel", "s_sethalt", "s_trap",
                                     "s_wakeup", "s_set_gpr_idx_off", "s_endpgm_saved", "s_wait_idle"};
        for (auto p : nops)
            if (n == p) return true;
        if (n == "s_barrier") {
            // Every thread of the group has to make this call (a PC waits at it for all of them;
            // the console only for the waves still running). In straight-line code the block's
            // condition is closed around it, so that waves the console code sends past the
            // barrier (those beyond a count, typically) make it too: without that the GPU was
            // reset, the rest of their group waiting for them.
            if (open_block >= 0 && opt.barrier_sync) {
                line("}");
                line("GroupMemoryBarrierWithGroupSync();");
                line(fmt("if (reach[%d]) {", open_block));
                return true;
            }
            if (coop) { line("atb_ = true;"); return true; }   // (the block ends here: see coop)
            line(opt.barrier_sync ? "GroupMemoryBarrierWithGroupSync();" : "GroupMemoryBarrier();");
            return true;
        }
        if (n == "s_sendmsg" || n == "s_sendmsghalt") {
            // MSG_DEALLOC_VGPRS and similar hints; stage-specific messages are handled by the stage.
            if ((in.simm16 & 0xf) == 3) return true;   // MSG_GS_DONE: nothing to do for compute
            if ((in.simm16 & 0xf) == 7) return true;   // MSG_ORDERED_PS_DONE: the end of an ordered section (see kPopsExitingWaveId)
            if ((in.simm16 & 0xf) == 9 && c.kind == ProgramKind::MS) {   // MSG_GS_ALLOC_REQ: m0 = primitives << 12 | vertices
                line("ms_counts_ = s124;");
                return true;
            }
            return fail(fmt("s_sendmsg %d at 0x%x not supported in this stage", in.simm16, in.offset));
        }
        return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
    }
    if (in.fmt == Fmt::SOPK) {
        std::string d = S(in.dst), k = fmt("0x%xu", uint32_t(in.simm16)), ku = fmt("0x%xu", uint32_t(in.simm16) & 0xffff);
        if (n == "s_movk_i32") write_s(in.dst, k);
        else if (n == "s_cmovk_i32") line(fmt("if (scc) %s = %s;", d.c_str(), k.c_str()));
        else if (n == "s_addk_i32") line(fmt("{ int r_ = int(%s) + int(%s); scc = ((int(%s) >= 0) == (int(%s) >= 0)) && ((r_ >= 0) != (int(%s) >= 0)); %s = uint(r_); }",
                                          d.c_str(), k.c_str(), d.c_str(), k.c_str(), d.c_str(), d.c_str()));
        else if (n == "s_mulk_i32") write_s(in.dst, fmt("uint(int(%s) * int(%s))", d.c_str(), k.c_str()));
        else if (starts(n, "s_cmpk_")) {
            std::string op = n.substr(7, 2), ty = n.substr(10);
            std::string x = ty == "i32" ? "int(" + d + ")" : d, y = ty == "i32" ? "int(" + k + ")" : ku;
            const char* o = op == "eq" ? "==" : op == "lg" ? "!=" : op == "gt" ? ">" : op == "ge" ? ">=" : op == "lt" ? "<" : "<=";
            line(fmt("scc = %s %s %s;", x.c_str(), o, y.c_str()));
        } else if (n == "s_getreg_b32") write_s(in.dst, "0u");
        else if (n == "s_setreg_b32" || n == "s_setreg_imm32_b32") {}
        else if (n == "s_waitcnt_vscnt" || n == "s_waitcnt_vmcnt" || n == "s_waitcnt_lgkmcnt" || n == "s_waitcnt_expcnt") {}   // memory is coherent per lane here
        else if (n == "s_call_b64") return fail("s_call_b64 not supported");
        else return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
        return true;
    }
    if (in.fmt == Fmt::SOPC) {
        if (starts(n, "s_cmp_")) {
            std::string op = n.substr(6, 2), ty = n.substr(9);
            const char* o = op == "eq" ? "==" : op == "lg" ? "!=" : op == "gt" ? ">" : op == "ge" ? ">=" : op == "lt" ? "<" : "<=";
            if (ty == "u64") line(fmt("scc = %s %s %s;", a64().c_str(), o, b64().c_str()));
            else if (ty == "i32") line(fmt("scc = int(%s) %s int(%s);", a().c_str(), o, b().c_str()));
            else line(fmt("scc = %s %s %s;", a().c_str(), o, b().c_str()));
            return true;
        }
        if (n == "s_bitcmp0_b32") { line(fmt("scc = ((%s >> (%s & 31u)) & 1u) == 0u;", a().c_str(), b().c_str())); return true; }
        if (n == "s_bitcmp1_b32") { line(fmt("scc = ((%s >> (%s & 31u)) & 1u) != 0u;", a().c_str(), b().c_str())); return true; }
        if (n == "s_bitcmp0_b64") { line(fmt("scc = ((%s >> (%s & 63u)) & 1u) == 0u;", a64().c_str(), b().c_str())); return true; }
        if (n == "s_bitcmp1_b64") { line(fmt("scc = ((%s >> (%s & 63u)) & 1u) != 0u;", a64().c_str(), b().c_str())); return true; }
        return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
    }
    if (in.fmt == Fmt::SOP1) {
        const unsigned d = in.dst;
        if (n == "s_mov_b32") { write_s(d, a()); return true; }
        if (n == "s_mov_b64") {
            if (d == src::kExecLo || d == 106 || in.src0 == src::kExecLo || in.src0 == 106)
                logic_mask(d, a(), rd_hi(in.src0, in), bit(in.src0), false);   // s_mov does not write SCC
            else
                line(fmt("%s = %s; %s = %s;", S(d).c_str(), a().c_str(), S(d + 1).c_str(), rd_hi(in.src0, in).c_str()));
            return true;
        }
        if (n == "s_cmov_b32") { line(fmt("if (scc) %s = %s;", S(d).c_str(), a().c_str())); sync_mask_from_int(d); return true; }
        if (n == "s_cmov_b64") { line(fmt("if (scc) { %s = %s; %s = %s; }", S(d).c_str(), a().c_str(), S(d + 1).c_str(), rd_hi(in.src0, in).c_str())); sync_mask_from_int(d); return true; }
        if (n == "s_not_b32") { line(fmt("%s = ~%s; scc = %s != 0u;", S(d).c_str(), a().c_str(), S(d).c_str())); sync_mask_from_int(d); return true; }
        if (n == "s_not_b64") { logic_mask(d, "~" + a(), "~" + rd_hi(in.src0, in), "!" + bit(in.src0)); return true; }
        if (n == "s_wqm_b64" || n == "s_wqm_b32") {
            std::string lo = fmt("wqm32(%s)", a().c_str()), hi = fmt("wqm32(%s)", rd_hi(in.src0, in).c_str());
            if (W == 64) line(fmt("{ uint lo_ = %s, hi_ = %s; %s = lo_; %s = hi_; scc = (lo_ | hi_) != 0u; }", lo.c_str(), hi.c_str(), S(d).c_str(), S(d + 1).c_str()));
            else line(fmt("{ uint lo_ = %s; %s = lo_; scc = lo_ != 0u; }", lo.c_str(), S(d).c_str()));
            // Helper lanes: on PC they always execute, so whole-quad mode makes this lane live.
            if (d == src::kExecLo && !dual) line("kexec = true;");
            else sync_mask_from_int(d);
            return true;
        }
        if (n == "s_brev_b32") { write_s(d, "reversebits(" + a() + ")"); return true; }
        if (n == "s_bcnt1_i32_b32") { line(fmt("%s = countbits(%s); scc = %s != 0u;", S(d).c_str(), a().c_str(), S(d).c_str())); return true; }
        if (n == "s_bcnt1_i32_b64") { line(fmt("%s = countbits(%s) + countbits(%s); scc = %s != 0u;", S(d).c_str(), a().c_str(), rd_hi(in.src0, in).c_str(), S(d).c_str())); return true; }
        if (n == "s_bcnt0_i32_b64") { line(fmt("%s = 64u - countbits(%s) - countbits(%s); scc = %s != 0u;", S(d).c_str(), a().c_str(), rd_hi(in.src0, in).c_str(), S(d).c_str())); return true; }
        if (n == "s_ff0_i32_b64") { write_s(d, fmt("ff1_64(~%s, ~%s)", a().c_str(), rd_hi(in.src0, in).c_str())); return true; }
        if (n == "s_bcnt0_i32_b32") { line(fmt("%s = 32u - countbits(%s); scc = %s != 0u;", S(d).c_str(), a().c_str(), S(d).c_str())); return true; }
        if (n == "s_ff1_i32_b32") { write_s(d, fmt("(%s == 0u ? 0xffffffffu : firstbitlow(%s))", a().c_str(), a().c_str())); return true; }
        if (n == "s_ff1_i32_b64") { write_s(d, fmt("ff1_64(%s, %s)", a().c_str(), rd_hi(in.src0, in).c_str())); return true; }
        if (n == "s_ff0_i32_b32") { write_s(d, fmt("(%s == 0xffffffffu ? 0xffffffffu : firstbitlow(~%s))", a().c_str(), a().c_str())); return true; }
        if (n == "s_flbit_i32_b32") { write_s(d, fmt("(%s == 0u ? 0xffffffffu : 31u - firstbithigh(%s))", a().c_str(), a().c_str())); return true; }
        if (n == "s_flbit_i32") { write_s(d, fmt("flbit_i32(%s)", a().c_str())); return true; }
        if (n == "s_sext_i32_i8") { write_s(d, fmt("uint(int(%s << 24) >> 24)", a().c_str())); return true; }
        if (n == "s_sext_i32_i16") { write_s(d, fmt("uint(int(%s << 16) >> 16)", a().c_str())); return true; }
        if (n == "s_abs_i32") { line(fmt("%s = uint(abs(int(%s))); scc = %s != 0u;", S(d).c_str(), a().c_str(), S(d).c_str())); return true; }
        if (n == "s_bitset0_b32") { write_s(d, fmt("%s & ~(1u << (%s & 31u))", S(d).c_str(), a().c_str())); return true; }
        if (n == "s_bitset1_b32") { write_s(d, fmt("%s | (1u << (%s & 31u))", S(d).c_str(), a().c_str())); return true; }
        if (n == "s_getpc_b64") {
            uint64_t a = addr_of(in.offset + 4);
            line(fmt("%s = 0x%xu; %s = 0x%xu;", S(d).c_str(), uint32_t(a), S(d + 1).c_str(), uint32_t(a >> 32)));
            return true;
        }
        if (has(n, "saveexec") || has(n, "wrexec")) {
            // s_<op>_saveexec: dst = exec; exec = op(src, exec). wrexec: exec = op(src, exec); dst = exec.
            std::string op = n.substr(2, n.find('_', 2) - 2);
            std::string sl = a(), sh = rd_hi(in.src0, in), sb = bit(in.src0);
            std::string il, ih, ib;
            auto mk = [&](const std::string& x, const std::string& e, bool isbool) -> std::string {
                const char* N = isbool ? "!" : "~";
                const char* A = isbool ? " && " : " & ";
                const char* O = isbool ? " || " : " | ";
                if (op == "and") return "(" + x + A + e + ")";
                if (op == "or") return "(" + x + O + e + ")";
                if (op == "xor") return isbool ? "(" + x + " != " + e + ")" : "(" + x + " ^ " + e + ")";
                if (op == "andn2") return "(" + x + A + N + e + ")";
                if (op == "orn2") return "(" + x + O + N + e + ")";
                if (op == "nand") return std::string(N) + "(" + x + A + e + ")";
                if (op == "nor") return std::string(N) + "(" + x + O + e + ")";
                if (op == "xnor") return isbool ? "(" + x + " == " + e + ")" : "~(" + x + " ^ " + e + ")";
                if (op == "andn1") return "(" + std::string(N) + x + A + e + ")";
                if (op == "orn1") return "(" + std::string(N) + x + O + e + ")";
                return "";
            };
            il = mk(sl, "s126", false);
            ih = mk(sh, "s127", false);
            ib = mk(sb, "kexec", true);
            if (il.empty()) return fail(n + " not supported");
            bool wr = has(n, "wrexec");
            if (dual) {
                line(fmt("{ uint ol_ = s126, oh_ = s127; uint lo_ = %s, hi_ = %s; s126 = lo_; s127 = hi_; scc = (lo_ | hi_) != 0u; %s = %s; %s = %s; }",
                         il.c_str(), ih.c_str(), S(d).c_str(), wr ? "lo_" : "ol_", S(d + 1).c_str(), wr ? "hi_" : "oh_"));
                sync_mask_from_int(126);
                if (d == 106) sync_mask_from_int(106);
                return true;
            }
            if (W == 64 || ends(n, "_b64"))
                line(fmt("{ uint ol_ = s126, oh_ = s127; uint lo_ = %s, hi_ = %s; bool k_ = %s; s126 = lo_; s127 = hi_; kexec = k_; scc = (lo_ | hi_) != 0u; %s = %s; %s = %s; }",
                         il.c_str(), ih.c_str(), ib.c_str(), S(d).c_str(), wr ? "lo_" : "ol_", S(d + 1).c_str(), wr ? "hi_" : "oh_"));
            else
                line(fmt("{ uint ol_ = s126; uint lo_ = %s; bool k_ = %s; s126 = lo_; kexec = k_; scc = lo_ != 0u; %s = %s; }", il.c_str(), ib.c_str(),
                         S(d).c_str(), wr ? "lo_" : "ol_"));
            if (d == 106) sync_mask_from_int(106);
            return true;
        }
        if (n == "s_swappc_b64" && c.kind == ProgramKind::VS && idx <= 1 && in.src0 == 4) {
            vs_fetch = true;   // call of the driver's fetch shader: the input assembler does it on PC
            return true;
        }
        if (n == "s_setpc_b64" || n == "s_swappc_b64") {   // block terminators: the dispatcher continues at the target
            line(fmt("{ uint tlo_ = %s, thi_ = %s;", S(in.src0).c_str(), S(in.src0 + 1).c_str()));
            if (n == "s_swappc_b64") {
                uint64_t ret = addr_of(in.offset + 4);
                line(fmt("  %s = 0x%xu; %s = 0x%xu;", S(in.dst).c_str(), uint32_t(ret), S(in.dst + 1).c_str(), uint32_t(ret >> 32)));
            }
            if (opt.raw_park && n == "s_swappc_b64" && block_of.count(in.offset + 4) && in.offset >= opt.raw_park_from && in.offset < opt.raw_park_to) {
                line("  uint t_ = jump_case(tlo_, thi_);");
                line("  if (t_ == 0xffffffffu) {   // outside this program: park the lanes");
                emit_park();
                line(fmt("    pc = %zuu;", block_of[in.offset + 4]));
                line(opt.raw_stats ? "  } else { pc = t_; if (kexec) dbg_add(27u); } }" : "  } else pc = t_; }");
            } else line(opt.raw_stats ? "  pc = jump_case(tlo_, thi_); if (pc == 0xffffffffu) dbg_set2(3u, tlo_, thi_); }" : "  pc = jump_case(tlo_, thi_); }");
            jumped = true;
            return true;
        }
        if (n == "s_movrels_b32" || n == "s_movreld_b32") return fail(n + " not supported");
        return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
    }
    // SOP2
    const unsigned d = in.dst;
    const std::string D = S(d);
    if (n == "s_add_u32") { line(fmt("{ uint x_ = %s, r_ = x_ + %s; %s = r_; scc = r_ < x_; }", a().c_str(), b().c_str(), D.c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_addc_u32") { line(fmt("{ uint x_ = %s, t_ = x_ + %s, r_ = t_ + (scc ? 1u : 0u); scc = t_ < x_ || r_ < t_; %s = r_; }", a().c_str(), b().c_str(), D.c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_sub_u32") { line(fmt("{ uint x_ = %s, y_ = %s; %s = x_ - y_; scc = y_ > x_; }", a().c_str(), b().c_str(), D.c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_subb_u32") { line(fmt("{ uint x_ = %s, y_ = %s, t_ = x_ - y_; bool c_ = scc; scc = y_ > x_ || (c_ && t_ == 0u); %s = t_ - (c_ ? 1u : 0u); }", a().c_str(), b().c_str(), D.c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_add_i32" || n == "s_sub_i32") {
        const char* o = n == "s_add_i32" ? "+" : "-";
        line(fmt("{ int x_ = int(%s), y_ = int(%s); int r_ = int(uint(x_) %s uint(y_)); scc = %s; %s = uint(r_); }", a().c_str(), b().c_str(), o,
                 n == "s_add_i32" ? "((x_ >= 0) == (y_ >= 0)) && ((r_ >= 0) != (x_ >= 0))" : "((x_ >= 0) != (y_ >= 0)) && ((r_ >= 0) != (x_ >= 0))", D.c_str()));
        sync_mask_from_int(d);
        return true;
    }
    if (n == "s_mul_i32") { write_s(d, fmt("(%s * %s)", a().c_str(), b().c_str())); return true; }
    if (n == "s_mul_hi_u32") { write_s(d, fmt("mulhi_u(%s, %s)", a().c_str(), b().c_str())); return true; }
    if (n == "s_mul_hi_i32") { write_s(d, fmt("uint(mulhi_i(int(%s), int(%s)))", a().c_str(), b().c_str())); return true; }
    if (n == "s_min_u32" || n == "s_max_u32") { const char* f = n == "s_min_u32" ? "min" : "max"; line(fmt("{ uint x_ = %s, y_ = %s; %s = %s(x_, y_); scc = %s; }", a().c_str(), b().c_str(), D.c_str(), f, n == "s_min_u32" ? "x_ < y_" : "x_ > y_")); return true; }
    if (n == "s_min_i32" || n == "s_max_i32") { const char* f = n == "s_min_i32" ? "min" : "max"; line(fmt("{ int x_ = int(%s), y_ = int(%s); %s = uint(%s(x_, y_)); scc = %s; }", a().c_str(), b().c_str(), D.c_str(), f, n == "s_min_i32" ? "x_ < y_" : "x_ > y_")); return true; }
    if (n == "s_cselect_b32") { write_s(d, fmt("(scc ? %s : %s)", a().c_str(), b().c_str())); return true; }
    if (n == "s_cselect_b64") {
        line(fmt("%s = scc ? %s : %s; %s = scc ? %s : %s;", D.c_str(), a().c_str(), b().c_str(), S(d + 1).c_str(), rd_hi(in.src0, in).c_str(), rd_hi(in.src1, in).c_str()));
        sync_mask_from_int(d);
        return true;
    }
    static const char* l32[][3] = {{"s_and_b32", "&", ""}, {"s_or_b32", "|", ""}, {"s_xor_b32", "^", ""}, {"s_andn2_b32", "& ~", ""},
                                   {"s_orn2_b32", "| ~", ""}, {"s_nand_b32", "&", "~"}, {"s_nor_b32", "|", "~"}, {"s_xnor_b32", "^", "~"}};
    static const char* lb[][3] = {{"and", "&&", ""}, {"or", "||", ""}, {"xor", "!=", ""}, {"andn2", "&& !", ""}, {"orn2", "|| !", ""},
                                  {"nand", "&&", "!"}, {"nor", "||", "!"}, {"xnor", "!=", "!"}};
    for (int k = 0; k < 8; k++) {
        std::string op = lb[k][0];
        if (n == "s_" + op + "_b64" || (W == 32 && n == "s_" + op + "_b32" && (d == src::kExecLo || d == 106 || in.src0 == src::kExecLo || in.src1 == src::kExecLo || in.src0 == 106 || in.src1 == 106))) {
            std::string il = fmt("%s(%s %s %s)", l32[k][2], a().c_str(), l32[k][1], b().c_str());
            std::string ih = fmt("%s(%s %s %s)", l32[k][2], rd_hi(in.src0, in).c_str(), l32[k][1], rd_hi(in.src1, in).c_str());
            std::string ib = fmt("%s(%s %s %s)", lb[k][2], bit(in.src0).c_str(), lb[k][1], bit(in.src1).c_str());
            logic_mask(d, il, ih, ib);
            return true;
        }
        if (n == l32[k][0]) {
            line(fmt("%s = %s(%s %s %s); scc = %s != 0u;", D.c_str(), l32[k][2], a().c_str(), l32[k][1], b().c_str(), D.c_str()));
            sync_mask_from_int(d);
            return true;
        }
    }
    if (n == "s_lshl_b32") { line(fmt("%s = %s << (%s & 31u); scc = %s != 0u;", D.c_str(), a().c_str(), b().c_str(), D.c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_lshr_b32") { line(fmt("%s = %s >> (%s & 31u); scc = %s != 0u;", D.c_str(), a().c_str(), b().c_str(), D.c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_ashr_i32") { line(fmt("%s = uint(int(%s) >> (%s & 31u)); scc = %s != 0u;", D.c_str(), a().c_str(), b().c_str(), D.c_str())); return true; }
    if (n == "s_lshl_b64") { line(fmt("{ uint64_t r_ = %s << (%s & 63u); %s = uint(r_); %s = uint(r_ >> 32); scc = r_ != 0; }", a64().c_str(), b().c_str(), D.c_str(), S(d + 1).c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_lshr_b64") { line(fmt("{ uint64_t r_ = %s >> (%s & 63u); %s = uint(r_); %s = uint(r_ >> 32); scc = r_ != 0; }", a64().c_str(), b().c_str(), D.c_str(), S(d + 1).c_str())); sync_mask_from_int(d); return true; }
    if (n == "s_ashr_i64") { line(fmt("{ uint64_t r_ = uint64_t(int64_t(%s) >> (%s & 63u)); %s = uint(r_); %s = uint(r_ >> 32); scc = r_ != 0; }", a64().c_str(), b().c_str(), D.c_str(), S(d + 1).c_str())); return true; }
    if (n == "s_bfm_b64") {   // ((1 << S0[5:0]) - 1) << S1[5:0], 64-bit
        line(fmt("{ uint w_ = %s & 63u, o_ = %s & 63u; uint64_t m_ = (w_ == 0u ? 0ull : (0xffffffffffffffffull >> (64u - w_))) << o_; %s = uint(m_); %s = uint(m_ >> 32); }",
                 a().c_str(), b().c_str(), D.c_str(), S(d + 1).c_str()));
        return true;
    }
    if (n == "s_bfm_b32") { write_s(d, fmt("(((1u << (%s & 31u)) - 1u) << (%s & 31u))", a().c_str(), b().c_str())); return true; }
    if (n == "s_bfe_u32") { line(fmt("%s = bfe_u(%s, %s & 31u, (%s >> 16) & 127u); scc = %s != 0u;", D.c_str(), a().c_str(), b().c_str(), b().c_str(), D.c_str())); return true; }
    if (n == "s_bfe_i32") { line(fmt("%s = uint(bfe_i(int(%s), %s & 31u, (%s >> 16) & 127u)); scc = %s != 0u;", D.c_str(), a().c_str(), b().c_str(), b().c_str(), D.c_str())); return true; }
    if (n == "s_bfe_u64") { w64(fmt("bfe_u64(%s, %s & 63u, (%s >> 16) & 127u)", a64().c_str(), b().c_str(), b().c_str())); line(fmt("scc = (%s | %s) != 0u;", D.c_str(), S(d + 1).c_str())); return true; }
    if (n == "s_absdiff_i32") { line(fmt("%s = uint(abs(int(%s) - int(%s))); scc = %s != 0u;", D.c_str(), a().c_str(), b().c_str(), D.c_str())); return true; }
    if (starts(n, "s_lshl") && ends(n, "_add_u32")) {
        int sh = n[6] - '0';
        line(fmt("{ uint x_ = %s, s_ = x_ << %d, r_ = s_ + %s; scc = (x_ >> %d) != 0u || r_ < s_; %s = r_; }", a().c_str(), sh, b().c_str(), 32 - sh, D.c_str()));
        return true;
    }
    if (n == "s_pack_ll_b32_b16") { write_s(d, fmt("((%s & 0xffffu) | (%s << 16))", a().c_str(), b().c_str())); return true; }
    if (n == "s_pack_lh_b32_b16") { write_s(d, fmt("((%s & 0xffffu) | (%s & 0xffff0000u))", a().c_str(), b().c_str())); return true; }
    if (n == "s_pack_hh_b32_b16") { write_s(d, fmt("((%s >> 16) | (%s & 0xffff0000u))", a().c_str(), b().c_str())); return true; }
    return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
}

// ---- SMEM --------------------------------------------------------------------------------
bool Translator::emit_smem(const Instr& in, size_t idx) {
    std::string n = in.name;
    unsigned cnt = has(n, "x16") ? 16 : has(n, "x8") ? 8 : has(n, "x4") ? 4 : has(n, "x2") ? 2 : 1;
    if (starts(n, "s_dcache") || n == "s_memtime" || n == "s_memrealtime" || n == "s_gl1_inv") {
        // HLSL has no GPU clock: a per-invocation counter that advances 1024 ticks per read keeps
        // time-based loops finite.
        if (n == "s_memtime" || n == "s_memrealtime") {
            line("memtime_ += 1024u;");
            write_s(in.sdata, "uint(memtime_)");
            write_s(in.sdata + 1, "uint(memtime_ >> 32)");
        }
        return true;
    }
    if (starts(n, "s_load_dword")) {
        size_t dk = dyn_alias.count(idx) ? dyn_alias[idx] : idx;   // merged loads write their group's variable
        if (dyn_loads.count(dk)) {
            std::string so = in.soffset == src::kNull ? std::string("0u") : rd(in.soffset, in);
            line(fmt("dyn%zu = %s + %uu + %s;", dk, S(in.sbase).c_str(), uint32_t(in.offset_imm), so.c_str()));
        }
        if (gm_sloads.count(idx)) {
            uses_gm = true;
            std::string so = in.soffset == src::kNull ? std::string("0u") : rd(in.soffset, in);
            line(fmt("{ uint lo_ = (%s & ~3u) + %uu + %s, hi_ = %s + (lo_ < %s ? 1u : 0u);", S(in.sbase).c_str(), uint32_t(in.offset_imm), so.c_str(),
                     S(in.sbase + 1).c_str(), S(in.sbase).c_str()));
            { std::vector<std::string> d; for (unsigned k = 0; k < cnt; k++) d.push_back(S(in.sdata + k)); gm_read_n(d); }
            line("}");
            return true;
        }
        auto it = sval.find(idx);
        for (unsigned k = 0; k < cnt; k++) write_s(in.sdata + k, it != sval.end() ? it->second[k] : std::string("0u"));
        return true;
    }
    if (starts(n, "s_buffer_load_dword")) {
        int r = res_of.count(idx) ? res_of[idx] : -1;
        if (r < 0 && dyn_sbuf.count(idx)) {   // descriptor words in SGPRs: base [47:0]
            uses_gm = true;
            unsigned cnt = has(n, "x16") ? 16 : has(n, "x8") ? 8 : has(n, "x4") ? 4 : has(n, "x2") ? 2 : 1;
            std::string off = in.soffset == src::kNull ? fmt("%uu", uint32_t(in.offset_imm)) : fmt("(%s + %uu)", rd(in.soffset, in).c_str(), uint32_t(in.offset_imm));
            line(fmt("{ uint lo_ = %s + (%s & ~3u), hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u);", S(in.sbase).c_str(), off.c_str(), S(in.sbase + 1).c_str(), S(in.sbase).c_str()));
            { std::vector<std::string> d; for (unsigned k = 0; k < cnt; k++) d.push_back(S(in.sdata + k)); gm_read_n(d); }
            line("}");
            return true;
        }
        if (r < 0) return false;
        const Res& R = res[r];
        if (R.dyn) return fail(n + ": scalar loads from resource arrays not supported");
        std::string off = in.soffset == src::kNull ? fmt("%uu", uint32_t(in.offset_imm)) : fmt("(%s + %uu)", rd(in.soffset, in).c_str(), uint32_t(in.offset_imm));
        if (R.addr_off) off = fmt("(%s + %uu)", off.c_str(), R.addr_off);
        // The destination may overlap the offset register: compute the address first.
        line(fmt("{ uint sa_ = %s;", off.c_str()));
        out << "  ";
        for (unsigned k = 0; k < cnt; k++) {
            std::string a = fmt("(sa_ + %uu)", 4 * k);
            if (R.kind == ResKind::CBuffer) write_s(in.sdata + k, fmt("cbload(%s_d, %s)", R.name.c_str(), a.c_str()));
            else if (R.kind == ResKind::RawBuf) write_s(in.sdata + k, fmt("%s.Load(%s & ~3u)", R.name.c_str(), a.c_str()));
            else if (R.kind == ResKind::StructBuf) write_s(in.sdata + k, fmt("%s[%s / %uu].w[(%s %% %uu) >> 2]", R.name.c_str(), a.c_str(), R.stride, a.c_str(), R.stride));
            else return fail(fmt("%s at 0x%x: scalar load from unsupported resource", in.name, in.offset));
        }
        line("}");
        return true;
    }
    return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
}

// ---- VALU --------------------------------------------------------------------------------
struct VTy {
    char t;   // 'f' f32, 'u' u32, 'i' i32, 'h' f16, 'w' u16, 's' i16, 'd' f64, 'U' u64, 'I' i64
};
char type_of_suffix(const std::string& s) {
    if (s == "f32") return 'f';
    if (s == "u32" || s == "b32") return 'u';
    if (s == "i32") return 'i';
    if (s == "f16") return 'h';
    if (s == "u16" || s == "b16") return 'w';
    if (s == "i16") return 's';
    if (s == "f64") return 'd';
    if (s == "u64" || s == "b64") return 'U';
    if (s == "i64") return 'I';
    return 0;
}

bool Translator::emit_valu(const Instr& in, size_t idx) {
    std::string n = in.name;
    if (n == "v_interp_p1ll_f16" || n == "v_interp_p1lv_f16" || n == "v_interp_p2_f16") return emit_interp16(in);
    const bool vop3 = in.fmt == Fmt::VOP3;
    // Sources: encodings (DPP/SDWA handled by src_bits).
    uint16_t se[3] = {in.src0, in.src1, in.src2};
    auto src_bits = [&](int k, char t) -> std::string {
        std::string v = rd(se[k], in, t);
        if (k == 0 && in.dpp) {
            const Dpp& d = in.dpp_f;
            if (d.dpp8) {
                std::string sel = "0u";
                for (int i = 7; i >= 0; i--) sel = fmt("((L & 7u) == %du ? %uu : %s)", i, d.sel[i], sel.c_str());
                v = fmt("xlane(%s, (L & ~7u) | %s)", v.c_str(), sel.c_str());
                uses_wave = true;
            } else if (d.ctrl <= 0xff) {   // quad_perm
                unsigned p[4] = {d.ctrl & 3u, (d.ctrl >> 2) & 3u, (d.ctrl >> 4) & 3u, (d.ctrl >> 6) & 3u};
                v = fmt("quadperm(%s, %uu, %uu, %uu, %uu)", v.c_str(), p[0], p[1], p[2], p[3]);
                uses_quad = true;
            } else {
                std::string srcl;
                unsigned c = d.ctrl;
                if (c >= 0x101 && c <= 0x10f) srcl = fmt("((L & 15u) + %uu < 16u ? L + %uu : 0xffffffffu)", c - 0x100, c - 0x100);   // row_shl
                else if (c >= 0x111 && c <= 0x11f) srcl = fmt("((L & 15u) >= %uu ? L - %uu : 0xffffffffu)", c - 0x110, c - 0x110);  // row_shr
                else if (c >= 0x121 && c <= 0x12f) srcl = fmt("((L & ~15u) | ((L + %uu) & 15u))", 16 - (c - 0x120));                // row_ror
                else if (c == 0x140) srcl = "((L & ~15u) | (15u - (L & 15u)))";       // row_mirror
                else if (c == 0x141) srcl = "((L & ~7u) | (7u - (L & 7u)))";          // row_half_mirror
                else if (c >= 0x150 && c <= 0x15f) srcl = fmt("((L & ~15u) | %uu)", c - 0x150);        // row_share
                else if (c >= 0x160 && c <= 0x16f) srcl = fmt("((L & ~15u) | ((L & 15u) ^ %uu))", c - 0x160);  // row_xmask
                else { fail(fmt("%s at 0x%x: DPP control 0x%x not supported", in.name, in.offset, c)); return "0u"; }
                v = fmt("xlane_bc(%s, %s, %s)", v.c_str(), srcl.c_str(), d.bound_ctrl ? "true" : "false");
                // Lanes DPP disables keep their destination: a source outside the row without
                // bound_ctrl, and lanes outside row_mask / bank_mask.
                std::string p;
                if (!d.bound_ctrl) p = fmt("(%s) != 0xffffffffu", srcl.c_str());
                if ((d.row_mask & 15) != 15) p += fmt("%s((%uu >> ((L >> 4) & 3u)) & 1u) != 0u", p.empty() ? "" : " && ", d.row_mask & 15u);
                if ((d.bank_mask & 15) != 15) p += fmt("%s((%uu >> ((L >> 2) & 3u)) & 1u) != 0u", p.empty() ? "" : " && ", d.bank_mask & 15u);
                lane_pred = p;
                uses_wave = true;
            }
        }
        if (in.sdwa) {
            uint8_t sel = k == 0 ? in.sdwa_f.src0_sel : in.sdwa_f.src1_sel;
            bool sx = k == 0 ? in.sdwa_f.sext0 : in.sdwa_f.sext1;
            if (sel < 6 && (k == 0 || in.fmt != Fmt::VOP1)) {
                unsigned sh = sel < 4 ? sel * 8 : (sel - 4) * 16, w = sel < 4 ? 8 : 16;
                if (sx) v = fmt("uint(int(%s << %u) >> %u)", v.c_str(), 32 - sh - w, 32 - w);
                else v = fmt("((%s >> %u) & 0x%xu)", v.c_str(), sh, (1u << w) - 1);
            }
        }
        return v;
    };
    // Typed source with float modifiers.
    auto src = [&](int k, char t) -> std::string {
        std::string b = src_bits(k, t);
        bool ab = false, ng = false;
        if (vop3) { ab = (in.abs >> k) & 1; ng = (in.neg >> k) & 1; }
        if (in.dpp && !in.dpp_f.dpp8) { ab = k == 0 ? in.dpp_f.abs0 : k == 1 ? in.dpp_f.abs1 : false; ng = k == 0 ? in.dpp_f.neg0 : k == 1 ? in.dpp_f.neg1 : false; }
        if (in.sdwa) { ab = k == 0 ? in.sdwa_f.abs0 : in.sdwa_f.abs1; ng = k == 0 ? in.sdwa_f.neg0 : in.sdwa_f.neg1; }
        std::string v;
        switch (t) {
        case 'f': v = "asfloat(" + b + ")"; break;
        case 'h': {
            bool hi = vop3 && ((in.opsel >> k) & 1);
            v = hi ? "f16tof32(" + b + " >> 16)" : "f16tof32(" + b + ")";
            break;
        }
        case 'i': v = "int(" + b + ")"; break;
        case 'w': v = vop3 && ((in.opsel >> k) & 1) ? "(" + b + " >> 16)" : "(" + b + " & 0xffffu)"; break;
        case 's': v = vop3 && ((in.opsel >> k) & 1) ? "(int(" + b + ") >> 16)" : "(int(" + b + " << 16) >> 16)"; break;
        case 'd':
            if (se[k] == src::kLiteral) v = fmt("asdouble(0u, 0x%xu)", in.literal);
            else v = "asdouble(" + (se[k] >= src::kFloatHalf && se[k] <= src::kInvTwoPi ? std::string("0u") : rd(se[k], in)) + ", " + rd_hi(se[k], in, 'd') + ")";
            break;
        case 'U': v = rd64(se[k], in); break;
        case 'I': v = "int64_t(" + rd64(se[k], in) + ")"; break;
        default: v = b;
        }
        if (t == 'f' || t == 'h' || t == 'd') {
            if (ab) v = "abs(" + v + ")";
            if (ng) v = "(-" + v + ")";
        }
        return v;
    };
    const unsigned dst = in.dst;
    // Result encoding with clamp/omod.
    auto fin = [&](char t, const std::string& e) -> std::string {
        std::string v = e;
        if (t == 'f' || t == 'h' || t == 'd') {
            uint8_t om = vop3 ? in.omod : in.sdwa ? in.sdwa_f.omod : 0;
            if (om == 1) v = "2.0 * (" + v + ")";
            if (om == 2) v = "4.0 * (" + v + ")";
            if (om == 3) v = "0.5 * (" + v + ")";
            if ((vop3 && in.clamp) || (in.sdwa && in.sdwa_f.clamp)) v = "saturate(" + v + ")";
        }
        switch (t) {
        case 'f': return "asuint(float(" + v + "))";
        case 'h': return "f16r(float(" + v + "))";
        case 'i': return "uint(" + v + ")";
        default: return "uint(" + v + ")";
        }
    };
    auto wv = [&](const std::string& e) {
        std::string v = e;
        if (in.sdwa && in.fmt != Fmt::VOPC && in.sdwa_f.dst_sel < 6) {
            unsigned sel = in.sdwa_f.dst_sel, sh = sel < 4 ? sel * 8 : (sel - 4) * 16, w = sel < 4 ? 8 : 16;
            unsigned m = ((1u << w) - 1) << sh;
            // dst_unused: 0 pads with zeros, 1 sign-extends the field upwards, 2 keeps the old bits.
            if (in.sdwa_f.dst_unused == 1) {
                unsigned up = 32 - sh - w;
                v = fmt("uint(int((((%s) << %u) & 0x%xu) << %u) >> %u)", v.c_str(), sh, m, up, up);
            } else {
                std::string keep = in.sdwa_f.dst_unused == 2 ? fmt("(%s & 0x%xu)", V(dst).c_str(), ~m) : "0u";
                v = fmt("(((%s) << %u) & 0x%xu) | %s", v.c_str(), sh, m, keep.c_str());
            }
        }
        write_v(dst, v);
    };
    // 16-bit results write the low half (VOP3 opsel bit 3 selects the high half); the other half is kept.
    auto wv16 = [&](const std::string& e) {
        bool hi = vop3 && ((in.opsel >> 3) & 1);
        if (in.sdwa) { wv(e); return; }
        if (hi) write_v(dst, fmt("((%s) << 16) | (%s & 0xffffu)", e.c_str(), V(dst).c_str()));
        else write_v(dst, fmt("((%s) & 0xffffu) | (%s & 0xffff0000u)", e.c_str(), V(dst).c_str()));
    };
    auto wv64 = [&](const std::string& e) {
        line(fmt("if (kexec) { uint64_t r_ = %s; %s = uint(r_); %s = uint(r_ >> 32); }", e.c_str(), V(dst).c_str(), V(dst + 1).c_str()));
    };
    // Mask operand (v_cndmask / carries): VOP3 src2 or VCC.
    auto mask_src = [&](int k) -> std::string {
        if (!vop3) return "kvcc";
        uint16_t e = se[k];
        if (e == 106) return "kvcc";
        if (e == src::kExecLo) return "kexec";
        if (e < kNumS) return mask_bool(e);
        if (e == src::kIntNeg1) return "true";
        if (e == src::kZero) return "false";
        return fmt("lanebit(%s, %s)", rd(e, in).c_str(), rd_hi(e, in).c_str());
    };
    // VOPC in VOP3 form writes its mask to the vdst field; VOP3b ops (carries) use sdst.
    auto mask_dst = [&]() -> unsigned { return vop3 ? (in.op < 256 ? in.dst : in.sdst) : 106u; };

    // Name parts.
    std::string base = n;
    std::string sfx = n.substr(n.rfind('_') + 1);
    char T = type_of_suffix(sfx);

    // ---- compares ----
    if (starts(n, "v_cmp")) {
        bool x = starts(n, "v_cmpx_");
        std::string rest = n.substr(x ? 7 : 6);   // "<cond>_<type>"
        std::string cond = rest.substr(0, rest.rfind('_'));
        char t = type_of_suffix(rest.substr(rest.rfind('_') + 1));
        std::string e;
        if (cond == "class") {
            e = t == 'd' ? fmt("fclass64(%s, %s)", src(0, 'd').c_str(), rd(se[1], in).c_str())
                         : fmt("fclass(%s, %s)", src(0, t == 'h' ? 'h' : 'f').c_str(), rd(se[1], in).c_str());
        } else {
            std::string a = src(0, t), b = src(1, t);
            if (t == 'f' || t == 'h' || t == 'd') {
                static const char* fc[][2] = {{"f", "false"}, {"lt", "a < b"}, {"eq", "a == b"}, {"le", "a <= b"}, {"gt", "a > b"},
                                              {"lg", "(a < b || a > b)"}, {"ge", "a >= b"}, {"o", "(a == a && b == b)"}, {"u", "(a != a || b != b)"},
                                              {"nge", "!(a >= b)"}, {"nlg", "!(a < b || a > b)"}, {"ngt", "!(a > b)"}, {"nle", "!(a <= b)"},
                                              {"neq", "!(a == b)"}, {"nlt", "!(a < b)"}, {"tru", "true"}, {"t", "true"}};
                for (auto& p : fc)
                    if (cond == p[0]) e = p[1];
            } else {
                static const char* ic[][2] = {{"f", "false"}, {"lt", "a < b"}, {"eq", "a == b"}, {"le", "a <= b"}, {"gt", "a > b"},
                                              {"ne", "a != b"}, {"lg", "a != b"}, {"ge", "a >= b"}, {"t", "true"}, {"tru", "true"}};
                for (auto& p : ic)
                    if (cond == p[0]) e = p[1];
            }
            if (e.empty()) return fail(n + " not supported");
            std::string r;
            for (char ch : e) {
                if (ch == 'a') r += "(" + a + ")";
                else if (ch == 'b') r += "(" + b + ")";
                else r += ch;
            }
            // Expressions like "false"/"true" contain letters a/b? Only via the operands above; keep literal words intact.
            if (e == "false" || e == "true") r = e;
            e = r;
        }
        if (x) {
            line(fmt("kexec = kexec && (%s);", e.c_str()));
            if (dual) line(fmt("%s = ballot2(kexec).x;", cp == 1 ? "s127" : "s126"));
            else line(W == 64 ? "{ uint2 b_ = ballot2(kexec); s126 = b_.x; s127 = b_.y; }" : "s126 = ballot2(kexec).x;");
        } else {
            unsigned m = in.sdwa && in.sdwa_f.sd ? in.sdwa_f.sdst : mask_dst();
            write_mask(m, fmt("kexec && (%s)", e.c_str()));
        }
        return true;
    }

    // ---- lane ops ----
    if (dual && (n == "v_readfirstlane_b32" || n == "v_readlane_b32") && in.src0 >= src::kVgpr0) {
        unsigned r = in.src0 - src::kVgpr0;
        std::string lane = n == "v_readlane_b32" ? fmt("(%s & 63u)", rd(se[1], in).c_str()) : std::string("first_lane64(s126, s127)");
        write_s(dst, fmt("readlane64(%s, %s, %s)", Vc(r, 0).c_str(), Vc(r, 1).c_str(), lane.c_str()));
        uses_wave = true;
        return true;
    }
    if (n == "v_swap_b32" && in.src0 >= src::kVgpr0) {   // exchange two VGPRs (active lanes)
        std::string a = V(dst), b = V(in.src0 - src::kVgpr0);
        line(fmt("if (kexec) { uint t_ = %s; %s = %s; %s = t_; }", a.c_str(), a.c_str(), b.c_str(), b.c_str()));
        return true;
    }
    if (n == "v_readfirstlane_b32") { write_s(dst, fmt("WaveReadLaneAt(%s, first_lane(kexec))", src_bits(0, 'u').c_str())); uses_wave = true; return true; }
    if (n == "v_readlane_b32") { write_s(dst, fmt("WaveReadLaneAt(%s, %s & %uu)", src_bits(0, 'u').c_str(), rd(se[1], in).c_str(), W - 1)); uses_wave = true; return true; }
    if (n == "v_writelane_b32") { line(fmt("if (L == (%s & %uu)) %s = %s;", rd(se[1], in).c_str(), W - 1, V(dst).c_str(), rd(se[0], in).c_str())); return true; }
    // Lane index within the console wave (LG: compute and mesh shaders derive it from the thread
    // index, so thread numbering stays right when the PC wave is narrower).
    if (n == "v_pack_b32_f16") {
        // Bit move: op_sel picks each source's half; neg flips and abs clears the f16 sign bit.
        auto h = [&](int k) {
            std::string b = src_bits(k, 'u');
            std::string v = ((in.opsel >> k) & 1) ? "(" + b + " >> 16)" : "(" + b + " & 0xffffu)";
            if ((in.abs >> k) & 1) v = "(" + v + " & 0x7fffu)";
            if ((in.neg >> k) & 1) v = "(" + v + " ^ 0x8000u)";
            return v;
        };
        wv(fmt("(%s | (%s << 16))", h(0).c_str(), h(1).c_str()));
        return true;
    }
    if (n == "v_mbcnt_lo_u32_b32") { wv(fmt("%s + countbits(%s & (LG < 32u ? ((1u << LG) - 1u) : 0xffffffffu))", rd(se[1], in).c_str(), rd(se[0], in).c_str())); return true; }
    if (n == "v_mbcnt_hi_u32_b32") { wv(fmt("%s + countbits(%s & (LG < 32u ? 0u : ((1u << (LG - 32u)) - 1u)))", rd(se[1], in).c_str(), rd(se[0], in).c_str())); return true; }
    if (n == "v_permlane16_b32" || n == "v_permlanex16_b32") {
        bool xx = n == "v_permlanex16_b32";
        std::string sel = fmt("(((L & 15u) < 8u ? (%s >> ((L & 7u) * 4u)) : (%s >> ((L & 7u) * 4u))) & 15u)", rd(se[1], in).c_str(), rd(se[2], in).c_str());
        std::string srcl = xx ? fmt("(((L & ~31u) | ((L & 16u) ^ 16u)) | %s)", sel.c_str()) : fmt("((L & ~15u) | %s)", sel.c_str());
        wv(fmt("xlane(%s, %s)", src_bits(0, 'u').c_str(), srcl.c_str()));
        uses_wave = true;
        return true;
    }
    if (n == "v_nop" || n == "v_pipeflush") return true;

    // ---- moves / selects ----
    if (n == "v_mov_b32") { wv(src_bits(0, 'u')); return true; }
    if (n == "v_movrels_b32" || n == "v_movreld_b32" || n == "v_movrelsd_b32" || n == "v_movrelsd_2_b32") {
        // VGPRs indexed by M0 (s124): expanded into switches over every VGPR once their count is known.
        if (n != "v_movreld_b32" && in.src0 < src::kVgpr0) return fail(n + " with a non-VGPR source");
        std::string sfx = V(0).substr(2);   // per-copy suffix in dual mode ("_0", "_1")
        unsigned so = in.src0 >= src::kVgpr0 ? in.src0 - src::kVgpr0 : 0;
        std::string val = n == "v_movreld_b32" ? src_bits(0, 'u') : "";
        // @@MOVREL{mode|dst|src|suffix|value}@@ (mode: s = indexed source, d = indexed destination, b = both)
        char mode = n == "v_movrels_b32" ? 's' : n == "v_movreld_b32" ? 'd' : 'b';
        line(fmt("@@MOVREL{%c|%u|%u|%s|%s}@@", mode, dst, so, sfx.c_str(), val.c_str()));
        return true;
    }
    if (n == "v_mov_b64") { wv64(rd64(se[0], in)); return true; }
    if (n == "v_cndmask_b32") {
        std::string m = mask_src(2);
        if (vop3 && (in.abs || in.neg)) wv(fmt("(%s ? %s : %s)", m.c_str(), fin('f', src(1, 'f')).c_str(), fin('f', src(0, 'f')).c_str()));
        else wv(fmt("(%s ? %s : %s)", m.c_str(), src_bits(1, 'u').c_str(), src_bits(0, 'u').c_str()));
        return true;
    }
    if (n == "v_cndmask_b16") { wv16(fmt("(%s ? %s : %s)", mask_src(2).c_str(), src(1, 'w').c_str(), src(0, 'w').c_str())); return true; }

    // ---- carries (VOP2 uses VCC, VOP3b uses sdst / src2) ----
    if (n == "v_add_co_u32" || n == "v_sub_co_u32" || n == "v_subrev_co_u32") {
        std::string a = src_bits(0, 'u'), b = src_bits(1, 'u');
        if (n == "v_subrev_co_u32") std::swap(a, b);
        bool add = n == "v_add_co_u32";
        line(fmt("{ uint x_ = %s, y_ = %s; uint r_ = %s; bool c_ = %s;", a.c_str(), b.c_str(), add ? "x_ + y_" : "x_ - y_", add ? "r_ < x_" : "y_ > x_"));
        line(fmt("  if (kexec) %s = r_;", V(dst).c_str()));
        out << "  ";
        write_mask(mask_dst(), "kexec && c_");
        line("}");
        return true;
    }
    if (n == "v_add_co_ci_u32" || n == "v_sub_co_ci_u32" || n == "v_subrev_co_ci_u32") {
        std::string a = src_bits(0, 'u'), b = src_bits(1, 'u');
        if (n == "v_subrev_co_ci_u32") std::swap(a, b);
        bool add = n == "v_add_co_ci_u32";
        std::string ci = mask_src(2);
        if (add) line(fmt("{ uint x_ = %s, t_ = x_ + %s; uint r_ = t_ + (%s ? 1u : 0u); bool c_ = t_ < x_ || r_ < t_;", a.c_str(), b.c_str(), ci.c_str()));
        else line(fmt("{ uint x_ = %s, y_ = %s, t_ = x_ - y_; bool ci_ = %s; uint r_ = t_ - (ci_ ? 1u : 0u); bool c_ = y_ > x_ || (ci_ && t_ == 0u);", a.c_str(), b.c_str(), ci.c_str()));
        line(fmt("  if (kexec) %s = r_;", V(dst).c_str()));
        out << "  ";
        write_mask(vop3 ? in.sdst : 106u, "kexec && c_");
        line("}");
        return true;
    }
    if (n == "v_mad_u64_u32" || n == "v_mad_i64_i32") {
        bool u = n == "v_mad_u64_u32";
        std::string x = rd(se[0], in), y = rd(se[1], in), cl = rd(se[2], in), ch = rd_hi(se[2], in);
        line(fmt("{ uint x_ = %s, y_ = %s; uint lo_ = x_ * y_, hi_ = %s; uint l2_ = lo_ + %s; hi_ += %s + (l2_ < lo_ ? 1u : 0u);",
                 x.c_str(), y.c_str(), u ? "mulhi_u(x_, y_)" : "uint(mulhi_i(int(x_), int(y_)))", cl.c_str(), ch.c_str()));
        line(fmt("  if (kexec) { %s = l2_; %s = hi_; } }", V(dst).c_str(), V(dst + 1).c_str()));
        if (in.sdst != src::kNull) write_mask(in.sdst, "false");
        return true;
    }
    // ---- division helpers: the scale/fmas/fixup sequence computes num/den; fixup does it exactly ----
    if (n == "v_div_scale_f32") { wv(fin('f', src(0, 'f'))); write_mask(in.sdst, "false"); return true; }
    if (n == "v_div_scale_f64") { wv64("asuint64(" + src(0, 'd') + ")"); write_mask(in.sdst, "false"); return true; }
    if (n == "v_div_fmas_f32") { wv(fin('f', fmt("mad(%s, %s, %s)", src(0, 'f').c_str(), src(1, 'f').c_str(), src(2, 'f').c_str()))); return true; }
    if (n == "v_div_fmas_f64") { wv64(fmt("asuint64(fma(%s, %s, %s))", src(0, 'd').c_str(), src(1, 'd').c_str(), src(2, 'd').c_str())); return true; }
    if (n == "v_div_fixup_f32") { wv(fin('f', fmt("(%s / %s)", src(2, 'f').c_str(), src(1, 'f').c_str()))); return true; }
    if (n == "v_div_fixup_f64") { wv64(fmt("asuint64(%s / %s)", src(2, 'd').c_str(), src(1, 'd').c_str())); return true; }
    if (n == "v_div_fixup_f16") { wv16(fin('h', fmt("(%s / %s)", src(2, 'h').c_str(), src(1, 'h').c_str()))); return true; }

    // ---- table of simple ops: name, result type, source types, expression (a b c d = sources/old dst, K = literal) ----
    struct Op { const char* name; char rt; const char* st; const char* e; };
    static const Op ops[] = {
        // f32
        {"v_add_f32", 'f', "ff", "a + b"}, {"v_sub_f32", 'f', "ff", "a - b"}, {"v_subrev_f32", 'f', "ff", "b - a"},
        {"v_mul_f32", 'f', "ff", "a * b"}, {"v_mul_legacy_f32", 'f', "ff", "mul_legacy(a, b)"},
        {"v_fma_f32", 'f', "fff", "mad(a, b, c)"}, {"v_mad_f32", 'f', "fff", "umad(a, b, c)"},
        {"v_fma_legacy_f32", 'f', "fff", "fma_legacy(a, b, c)"}, {"v_mad_legacy_f32", 'f', "fff", "mad_legacy(a, b, c)"},
        {"v_fmac_f32", 'f', "ffD", "mad(a, b, d)"}, {"v_mac_f32", 'f', "ffD", "umad(a, b, d)"},
        {"v_fmac_legacy_f32", 'f', "ffD", "fma_legacy(a, b, d)"}, {"v_mac_legacy_f32", 'f', "ffD", "mad_legacy(a, b, d)"},
        {"v_madak_f32", 'f', "ff", "umad(a, b, Kf)"}, {"v_fmaak_f32", 'f', "ff", "mad(a, b, Kf)"},
        {"v_madmk_f32", 'f', "ff", "umad(a, Kf, b)"}, {"v_fmamk_f32", 'f', "ff", "mad(a, Kf, b)"},
        {"v_min_f32", 'f', "ff", "min(a, b)"}, {"v_max_f32", 'f', "ff", "max(a, b)"},
        {"v_min3_f32", 'f', "fff", "min(min(a, b), c)"}, {"v_max3_f32", 'f', "fff", "max(max(a, b), c)"},
        {"v_med3_f32", 'f', "fff", "med3f(a, b, c)"},
        {"v_rcp_f32", 'f', "f", "rcp(a)"}, {"v_rcp_iflag_f32", 'f', "f", "rcp(a)"}, {"v_rsq_f32", 'f', "f", "rsqrt(a)"},
        {"v_sqrt_f32", 'f', "f", "sqrt(a)"}, {"v_exp_f32", 'f', "f", "exp2(a)"}, {"v_log_f32", 'f', "f", "log2(a)"},
        {"v_sin_f32", 'f', "f", "sin(a * 6.28318530717958647692)"}, {"v_cos_f32", 'f', "f", "cos(a * 6.28318530717958647692)"},
        {"v_fract_f32", 'f', "f", "frac(a)"}, {"v_floor_f32", 'f', "f", "floor(a)"}, {"v_ceil_f32", 'f', "f", "ceil(a)"},
        {"v_trunc_f32", 'f', "f", "trunc(a)"}, {"v_rndne_f32", 'f', "f", "round(a)"},
        {"v_ldexp_f32", 'f', "fi", "ldexp(a, float(b))"}, {"v_frexp_mant_f32", 'f', "f", "frexp_mant(a)"},
        {"v_frexp_exp_i32_f32", 'i', "f", "frexp_exp(a)"},
        {"v_cvt_f32_u32", 'f', "u", "float(a)"}, {"v_cvt_f32_i32", 'f', "i", "float(a)"},
        {"v_cvt_u32_f32", 'u', "f", "cvt_u32(a)"}, {"v_cvt_i32_f32", 'i', "f", "cvt_i32(a)"},
        {"v_cvt_flr_i32_f32", 'i', "f", "cvt_i32(floor(a))"}, {"v_cvt_rpi_i32_f32", 'i', "f", "cvt_i32(floor(a + 0.5))"},
        {"v_cvt_f32_ubyte0", 'f', "u", "float(a & 0xffu)"}, {"v_cvt_f32_ubyte1", 'f', "u", "float((a >> 8) & 0xffu)"},
        {"v_cvt_f32_ubyte2", 'f', "u", "float((a >> 16) & 0xffu)"}, {"v_cvt_f32_ubyte3", 'f', "u", "float(a >> 24)"},
        {"v_cvt_f32_f16", 'f', "h", "a"}, {"v_cvt_f16_f32", 'h', "f", "a"},
        {"v_cvt_pkrtz_f16_f32", 'u', "ff", "pkrtz(a, b)"}, {"v_cvt_pknorm_u16_f32", 'u', "ff", "pknorm_u16(a, b)"},
        {"v_cvt_pknorm_i16_f32", 'u', "ff", "pknorm_i16(a, b)"}, {"v_cvt_pk_u16_u32", 'u', "uu", "(min(a, 0xffffu) | (min(b, 0xffffu) << 16))"},
        {"v_cvt_pk_i16_i32", 'u', "ii", "((uint(clamp(a, -32768, 32767)) & 0xffffu) | (uint(clamp(b, -32768, 32767)) << 16))"},
        {"v_cvt_pk_u8_f32", 'u', "fuu", "pk_u8(a, b, c)"},
        {"v_cvt_off_f32_i4", 'f', "u", "(float(int(a << 28) >> 28) / 16.0)"},
        {"v_cubeid_f32", 'f', "fff", "cube_id(a, b, c)"}, {"v_cubesc_f32", 'f', "fff", "cube_sc(a, b, c)"},
        {"v_cubetc_f32", 'f', "fff", "cube_tc(a, b, c)"}, {"v_cubema_f32", 'f', "fff", "cube_ma(a, b, c)"},
        {"v_dot2c_f32_f16", 'f', "uuD", "dot2_f16(a, b, d)"}, {"v_dot2_f32_f16", 'f', "uuf", "dot2_f16(a, b, c)"},
        {"v_dot4c_i32_i8", 'u', "uuD", "dot4_i8(a, b, d)"}, {"v_pk_fmac_f16", 'u', "uuD", "pk_fmac(a, b, d)"},
        // integer
        {"v_add_nc_u32", 'u', "uu", "a + b"}, {"v_sub_nc_u32", 'u', "uu", "a - b"}, {"v_subrev_nc_u32", 'u', "uu", "b - a"},
        {"v_add_u32", 'u', "uu", "a + b"}, {"v_sub_u32", 'u', "uu", "a - b"}, {"v_subrev_u32", 'u', "uu", "b - a"},
        {"v_add_nc_i32", 'u', "uu", "a + b"}, {"v_sub_nc_i32", 'u', "uu", "a - b"},
        {"v_mul_lo_u32", 'u', "uu", "a * b"}, {"v_mul_lo_i32", 'u', "uu", "a * b"},
        {"v_mul_hi_u32", 'u', "uu", "mulhi_u(a, b)"},
        {"v_mul_hi_i32", 'u', "ii", "uint(mulhi_i(a, b))"},
        {"v_mul_u32_u24", 'u', "uu", "(a & 0xffffffu) * (b & 0xffffffu)"},
        {"v_mul_i32_i24", 'u', "uu", "uint((int(a << 8) >> 8) * (int(b << 8) >> 8))"},
        {"v_mul_hi_u32_u24", 'u', "uu", "mulhi_u(a & 0xffffffu, b & 0xffffffu)"},
        {"v_mul_hi_i32_i24", 'u', "uu", "uint(mulhi_i(int(a << 8) >> 8, int(b << 8) >> 8))"},
        {"v_mad_u32_u24", 'u', "uuu", "(a & 0xffffffu) * (b & 0xffffffu) + c"},
        {"v_mad_i32_i24", 'u', "uuu", "uint((int(a << 8) >> 8) * (int(b << 8) >> 8)) + c"},
        {"v_mad_u32_u16", 'u', "uuu", "(a & 0xffffu) * (b & 0xffffu) + c"},
        {"v_mac_u32_u24", 'u', "uuD", "(a & 0xffffffu) * (b & 0xffffffu) + d"},
        {"v_lshlrev_b32", 'u', "uu", "b << (a & 31u)"}, {"v_lshrrev_b32", 'u', "uu", "b >> (a & 31u)"},
        {"v_ashrrev_i32", 'u', "uu", "uint(int(b) >> (a & 31u))"},
        {"v_lshl_b32", 'u', "uu", "a << (b & 31u)"}, {"v_lshr_b32", 'u', "uu", "a >> (b & 31u)"}, {"v_ashr_i32", 'u', "uu", "uint(int(a) >> (b & 31u))"},
        {"v_lshl_add_u32", 'u', "uuu", "(a << (b & 31u)) + c"}, {"v_add_lshl_u32", 'u', "uuu", "(a + b) << (c & 31u)"},
        {"v_lshl_or_b32", 'u', "uuu", "(a << (b & 31u)) | c"}, {"v_and_or_b32", 'u', "uuu", "(a & b) | c"},
        {"v_or3_b32", 'u', "uuu", "a | b | c"}, {"v_add3_u32", 'u', "uuu", "a + b + c"}, {"v_xad_u32", 'u', "uuu", "(a ^ b) + c"},
        {"v_xor3_b32", 'u', "uuu", "a ^ b ^ c"},
        {"v_and_b32", 'u', "uu", "a & b"}, {"v_or_b32", 'u', "uu", "a | b"}, {"v_xor_b32", 'u', "uu", "a ^ b"},
        {"v_xnor_b32", 'u', "uu", "~(a ^ b)"}, {"v_not_b32", 'u', "u", "~a"},
        {"v_bfe_u32", 'u', "uuu", "bfe_u(a, b & 31u, c & 31u)"}, {"v_bfe_i32", 'u', "uuu", "uint(bfe_i(int(a), b & 31u, c & 31u))"},
        {"v_bfi_b32", 'u', "uuu", "(a & b) | (~a & c)"}, {"v_bfm_b32", 'u', "uu", "(((1u << (a & 31u)) - 1u) << (b & 31u))"},
        {"v_alignbit_b32", 'u', "uuu", "uint(((uint64_t(a) << 32) | uint64_t(b)) >> (c & 31u))"},
        {"v_alignbyte_b32", 'u', "uuu", "uint(((uint64_t(a) << 32) | uint64_t(b)) >> ((c & 3u) * 8u))"},
        {"v_min_u32", 'u', "uu", "min(a, b)"}, {"v_max_u32", 'u', "uu", "max(a, b)"},
        {"v_min_i32", 'u', "ii", "uint(min(a, b))"}, {"v_max_i32", 'u', "ii", "uint(max(a, b))"},
        {"v_min3_u32", 'u', "uuu", "min(min(a, b), c)"}, {"v_max3_u32", 'u', "uuu", "max(max(a, b), c)"},
        {"v_min3_i32", 'u', "iii", "uint(min(min(a, b), c))"}, {"v_max3_i32", 'u', "iii", "uint(max(max(a, b), c))"},
        {"v_med3_u32", 'u', "uuu", "max(min(a, b), min(max(a, b), c))"}, {"v_med3_i32", 'u', "iii", "uint(max(min(a, b), min(max(a, b), c)))"},
        {"v_bcnt_u32_b32", 'u', "uu", "countbits(a) + b"}, {"v_bfrev_b32", 'u', "u", "reversebits(a)"},
        {"v_ffbh_u32", 'u', "u", "(a == 0u ? 0xffffffffu : 31u - firstbithigh(a))"},
        {"v_ffbl_b32", 'u', "u", "(a == 0u ? 0xffffffffu : firstbitlow(a))"},
        {"v_ffbh_i32", 'u', "u", "flbit_i32(a)"},
        {"v_perm_b32", 'u', "uuu", "perm_b32(a, b, c)"},
        {"v_sad_u32", 'u', "uuu", "(max(a, b) - min(a, b)) + c"},
        {"v_lerp_u8", 'u', "uuu", "lerp_u8(a, b, c)"},
        {"v_msad_u8", 'u', "uuu", "msad_u8(a, b, c)"}, {"v_sad_u8", 'u', "uuu", "sad_u8(a, b, c)"},
        // f64 / 64-bit
        {"v_add_f64", 'd', "dd", "a + b"}, {"v_mul_f64", 'd', "dd", "a * b"}, {"v_fma_f64", 'd', "ddd", "fma(a, b, c)"},
        {"v_min_f64", 'd', "dd", "min(a, b)"}, {"v_max_f64", 'd', "dd", "max(a, b)"}, {"v_rcp_f64", 'd', "d", "(1.0 / a)"},
        {"v_rsq_f64", 'd', "d", "(1.0 / sqrt(float(a)))"}, {"v_sqrt_f64", 'd', "d", "sqrt(float(a))"},
        {"v_cvt_f64_f32", 'd', "f", "double(a)"}, {"v_cvt_f32_f64", 'f', "d", "float(a)"},
        {"v_cvt_f64_i32", 'd', "i", "double(a)"}, {"v_cvt_f64_u32", 'd', "u", "double(a)"},
        {"v_cvt_i32_f64", 'i', "d", "cvt_i32(float(a))"}, {"v_cvt_u32_f64", 'u', "d", "cvt_u32(float(a))"},
        {"v_floor_f64", 'd', "d", "double(floor(float(a)))"}, {"v_fract_f64", 'd', "d", "(a - double(floor(float(a))))"},
        {"v_lshlrev_b64", 'U', "uU", "b << (a & 63u)"}, {"v_lshrrev_b64", 'U', "uU", "b >> (a & 63u)"},
        {"v_ashrrev_i64", 'U', "uI", "uint64_t(b >> (a & 63u))"},
        // f16
        {"v_add_f16", 'h', "hh", "a + b"}, {"v_sub_f16", 'h', "hh", "a - b"}, {"v_subrev_f16", 'h', "hh", "b - a"},
        {"v_mul_f16", 'h', "hh", "a * b"}, {"v_fma_f16", 'h', "hhh", "mad(a, b, c)"}, {"v_mad_f16", 'h', "hhh", "umad(a, b, c)"},
        {"v_fmac_f16", 'h', "hhE", "mad(a, b, d)"}, {"v_mac_f16", 'h', "hhE", "umad(a, b, d)"},
        {"v_fmaak_f16", 'h', "hh", "mad(a, b, Kh)"}, {"v_fmamk_f16", 'h', "hh", "mad(a, Kh, b)"},
        {"v_min_f16", 'h', "hh", "min(a, b)"}, {"v_max_f16", 'h', "hh", "max(a, b)"},
        {"v_min3_f16", 'h', "hhh", "min(min(a, b), c)"}, {"v_max3_f16", 'h', "hhh", "max(max(a, b), c)"}, {"v_med3_f16", 'h', "hhh", "med3f(a, b, c)"},
        {"v_rcp_f16", 'h', "h", "rcp(a)"}, {"v_rsq_f16", 'h', "h", "rsqrt(a)"}, {"v_sqrt_f16", 'h', "h", "sqrt(a)"},
        {"v_exp_f16", 'h', "h", "exp2(a)"}, {"v_log_f16", 'h', "h", "log2(a)"}, {"v_fract_f16", 'h', "h", "frac(a)"},
        {"v_floor_f16", 'h', "h", "floor(a)"}, {"v_ceil_f16", 'h', "h", "ceil(a)"}, {"v_trunc_f16", 'h', "h", "trunc(a)"},
        {"v_rndne_f16", 'h', "h", "round(a)"}, {"v_sin_f16", 'h', "h", "sin(a * 6.28318530717958647692)"},
        {"v_cos_f16", 'h', "h", "cos(a * 6.28318530717958647692)"}, {"v_ldexp_f16", 'h', "hi", "ldexp(a, float(b))"},
        {"v_cvt_f16_u16", 'h', "w", "float(a)"}, {"v_cvt_f16_i16", 'h', "s", "float(a)"},
        {"v_cvt_u16_f16", 'W', "h", "min(cvt_u32(a), 0xffffu)"}, {"v_cvt_i16_f16", 'S', "h", "uint(clamp(cvt_i32(a), -32768, 32767))"},
        // 16-bit integer
        {"v_add_nc_u16", 'W', "ww", "a + b"}, {"v_sub_nc_u16", 'W', "ww", "a - b"}, {"v_mul_lo_u16", 'W', "ww", "a * b"},
        {"v_add_u16", 'W', "ww", "a + b"}, {"v_sub_u16", 'W', "ww", "a - b"},
        {"v_lshlrev_b16", 'W', "ww", "b << (a & 15u)"}, {"v_lshrrev_b16", 'W', "ww", "b >> (a & 15u)"},
        {"v_ashrrev_i16", 'W', "ws", "uint(b >> (a & 15u))"},
        {"v_min_u16", 'W', "ww", "min(a, b)"}, {"v_max_u16", 'W', "ww", "max(a, b)"},
        {"v_min_i16", 'S', "ss", "uint(min(a, b))"}, {"v_max_i16", 'S', "ss", "uint(max(a, b))"},
        {"v_mad_u16", 'W', "www", "a * b + c"}, {"v_mad_i16", 'S', "sss", "uint(a * b + c)"},
    };
    for (auto& o : ops) {
        if (n != o.name) continue;
        std::string s[3], d = V(dst);
        char dt = o.rt;
        int ns = 0;
        bool acc = false, acch = false;
        for (int k = 0; o.st[k]; k++) {
            char t = o.st[k];
            if (t == 'D') { acc = true; continue; }
            if (t == 'E') { acch = true; continue; }
            s[ns] = src(ns, t);
            ns++;
        }
        std::string e = o.e, r;
        for (size_t i = 0; i < e.size(); i++) {
            char ch = e[i];
            bool word = (i > 0 && (isalnum(uint8_t(e[i - 1])) || e[i - 1] == '_')) || (i + 1 < e.size() && (isalnum(uint8_t(e[i + 1])) || e[i + 1] == '_'));
            if (!word && ch >= 'a' && ch <= 'c') r += "(" + s[ch - 'a'] + ")";
            else if (!word && ch == 'd') r += acc ? "asfloat(" + d + ")" : acch ? "f16tof32(" + d + ")" : d;
            else if (ch == 'K' && i + 1 < e.size() && (e[i + 1] == 'f' || e[i + 1] == 'h')) {
                r += e[i + 1] == 'f' ? fmt("asfloat(0x%xu)", in.literal) : fmt("f16tof32(0x%xu)", in.literal & 0xffff);
                i++;
            } else r += ch;
        }
        if (dt == 'd') wv64("asuint64(" + r + ")");
        else if (dt == 'U') wv64(r);
        else if (dt == 'h') wv16(fin('h', r));
        else if (dt == 'W' || dt == 'S') wv16("uint(" + r + ") & 0xffffu");
        else if (dt == 'f' && !has(o.e, "mad(") && getenv("XSL_PRECISE_OPS")) {
            // The console does not contract separate multiplies and adds: keep the PC compiler
            // from fusing this result into a later operation (real FMAs use mad and may fuse).
            // Opt-in (XSL_PRECISE_OPS): DXC 1.9 crashes on large shaders with thousands of these.
            std::string f = fin('f', r);   // asuint(float(...))
            line(fmt("{ precise float pf_ = asfloat(%s);", f.c_str()));
            wv("asuint(pf_)");
            line("}");
        } else wv(fin(dt, r));
        return true;
    }
    // Packed 16-bit (VOP3P).
    if (in.fmt == Fmt::VOP3P) {
        auto half = [&](int k, bool hi_half) -> std::string {
            std::string b = rd(se[k], in, 'h');
            bool sel = hi_half ? ((in.opsel_hi >> k) & 1) : ((in.opsel >> k) & 1);
            bool ng = hi_half ? ((in.neg_hi >> k) & 1) : ((in.neg >> k) & 1);
            std::string v = sel ? "(" + b + " >> 16)" : "(" + b + " & 0xffffu)";
            return v + (ng ? "^NEG" : "");
        };
        auto fh = [&](int k, bool h) {
            std::string v = half(k, h);
            bool ng = has(v, "^NEG");
            if (ng) v = v.substr(0, v.size() - 4);
            return ng ? "(-f16tof32(" + v + "))" : "f16tof32(" + v + ")";
        };
        auto ih = [&](int k, bool h) {
            std::string v = half(k, h);
            if (has(v, "^NEG")) v = v.substr(0, v.size() - 4);
            return v;
        };
        if (n == "v_dot4_i32_i8" || n == "v_dot4_u32_u8") {
            // Four byte products summed onto the third source. op_sel chooses each source's 16-bit
            // halves as for the other packed operations (measured by others on the hardware: with
            // op_sel 2 and op_sel_hi 5 the second source's halves are exchanged).
            if (n == "v_dot4_u32_u8") return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
            uses_dot4 = true;
            auto whole = [&](int k) { return "(" + ih(k, false) + " | (" + ih(k, true) + " << 16))"; };
            write_v(dst, fmt("dot4_i8(%s, %s, %s)", whole(0).c_str(), whole(1).c_str(), rd(se[2], in, 'u').c_str()));
            return true;
        }
        if (n == "v_fma_mix_f32" || n == "v_fma_mixlo_f16" || n == "v_fma_mixhi_f16") {
            // Each source is f32 or f16 according to opsel_hi; opsel picks the f16 half.
            auto ms = [&](int k) -> std::string {
                bool is16 = (in.opsel_hi >> k) & 1;
                std::string b = rd(se[k], in, is16 ? 'h' : 'f');
                std::string v = is16 ? (((in.opsel >> k) & 1) ? "f16tof32(" + b + " >> 16)" : "f16tof32(" + b + ")") : "asfloat(" + b + ")";
                if ((in.neg_hi >> k) & 1) v = "abs(" + v + ")";
                if ((in.neg >> k) & 1) v = "(-" + v + ")";
                return v;
            };
            std::string e = fmt("mad(%s, %s, %s)", ms(0).c_str(), ms(1).c_str(), ms(2).c_str());
            if (in.clamp) e = "saturate(" + e + ")";
            if (n == "v_fma_mix_f32") write_v(dst, "asuint(float(" + e + "))");
            else if (n == "v_fma_mixlo_f16") write_v(dst, fmt("(f16r(%s) & 0xffffu) | (%s & 0xffff0000u)", e.c_str(), V(dst).c_str()));
            else write_v(dst, fmt("(f16r(%s) << 16) | (%s & 0xffffu)", e.c_str(), V(dst).c_str()));
            return true;
        }
        // Kind: 'f' f16, 'u' unsigned 16-bit, 's' signed 16-bit (halves sign-extended).
        struct PO { const char* name; char f; const char* e; };
        static const PO po[] = {{"v_pk_add_f16", 'f', "a + b"}, {"v_pk_mul_f16", 'f', "a * b"}, {"v_pk_fma_f16", 'f', "mad(a, b, c)"},
                                {"v_pk_min_f16", 'f', "min(a, b)"}, {"v_pk_max_f16", 'f', "max(a, b)"},
                                {"v_pk_add_u16", 'u', "a + b"}, {"v_pk_sub_u16", 'u', "a - b"}, {"v_pk_mul_lo_u16", 'u', "a * b"},
                                {"v_pk_add_i16", 'u', "a + b"}, {"v_pk_sub_i16", 'u', "a - b"}, {"v_pk_mad_u16", 'u', "a * b + c"},
                                {"v_pk_lshlrev_b16", 'u', "b << (a & 15u)"}, {"v_pk_lshrrev_b16", 'u', "b >> (a & 15u)"},
                                {"v_pk_ashrrev_i16", 's', "b >> (a & 15)"},
                                {"v_pk_min_u16", 'u', "min(a, b)"}, {"v_pk_max_u16", 'u', "max(a, b)"},
                                {"v_pk_min_i16", 's', "min(a, b)"}, {"v_pk_max_i16", 's', "max(a, b)"},
                                {"v_pk_mad_i16", 's', "a * b + c"}};
        for (auto& p : po) {
            if (n != p.name) continue;
            std::string lohi[2];
            for (int h = 0; h < 2; h++) {
                std::string e = p.e, r;
                for (size_t i = 0; i < e.size(); i++) {
                    char ch = e[i];
                    bool word = (i > 0 && isalnum(uint8_t(e[i - 1]))) || (i + 1 < e.size() && isalnum(uint8_t(e[i + 1])));
                    if (!word && ch >= 'a' && ch <= 'c')
                        r += "(" + (p.f == 'f' ? fh(ch - 'a', h) : p.f == 's' ? "(int(" + ih(ch - 'a', h) + " << 16) >> 16)" : ih(ch - 'a', h)) + ")";
                    else r += ch;
                }
                lohi[h] = p.f == 'f' ? (in.clamp ? "f16r(saturate(" + r + "))" : "f16r(" + r + ")") : "(uint(" + r + ") & 0xffffu)";
            }
            write_v(dst, fmt("(%s & 0xffffu) | (%s << 16)", lohi[0].c_str(), lohi[1].c_str()));
            return true;
        }
    }
    return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
}

// ---- buffers -----------------------------------------------------------------------------
// Typed buffer access on a raw or structured view: the instruction's format field (GFX10 unified
// format, names generated from LLVM: buf_formats.inc) says how the element is packed and converted.
namespace {
struct BufFmt { unsigned id; const char* name; };
const BufFmt kBufFmts[] = {
#include "buf_formats.inc"
};
}  // namespace
bool Translator::emit_tbuffer_raw(const Instr& in, const Res& R, const std::string& RN, const std::string& addr,
                                  const std::function<std::string(const std::string&)>& word) {
    std::string n = in.name;
    const char* fname = nullptr;
    for (auto& f : kBufFmts) if (f.id == in.format) fname = f.name;
    if (!fname) return fail(fmt("%s: buffer format %u unknown", in.name, in.format));
    if (in.d16) return fail(n + " (16-bit data) on a raw buffer not supported");
    std::string fs = fname;   // e.g. "16_16_UNORM", "10_10_10_2_UNORM", "32_32_FLOAT"
    size_t us = fs.rfind('_');
    std::string type = fs.substr(us + 1);
    std::vector<unsigned> bits;
    for (size_t p = 0; p < us;) {
        size_t q = fs.find('_', p);
        if (q == std::string::npos || q > us) q = us;
        bits.push_back((unsigned)atoi(fs.substr(p, q - p).c_str()));
        p = q + 1;
    }
    if (bits.empty() || bits.size() > 4) return fail(fmt("%s: buffer format %s not supported", in.name, fname));
    bool packed = false;
    for (unsigned b : bits) if (b != bits[0]) packed = true;
    unsigned total = 0;
    for (unsigned b : bits) total += b;
    if (packed && total != 32) return fail(fmt("%s: buffer format %s not supported", in.name, fname));
    if (!packed && bits[0] != 8 && bits[0] != 16 && bits[0] != 32) return fail(fmt("%s: buffer format %s not supported", in.name, fname));
    if (type == "FLOAT" && (packed || bits[0] == 8)) return fail(fmt("%s: buffer format %s not supported", in.name, fname));
    unsigned cnt = ends(n, "_xyzw") ? 4 : ends(n, "_xyz") ? 3 : ends(n, "_xy") ? 2 : 1;
    auto dw = [&](const std::string& a) { return R.kind == ResKind::RawBuf ? fmt("%s.Load((%s) & ~3u)", RN.c_str(), a.c_str()) : word(fmt("((%s) & ~3u)", a.c_str())); };
    // Bit position of component c, relative to the element start (components are packed from bit 0).
    auto pos = [&](unsigned c) { unsigned p = 0; for (unsigned k = 0; k < c; k++) p += bits[k]; return p; };
    bool load = starts(n, "tbuffer_load");
    line(fmt("if (kexec) { uint a_ = %s;", addr.c_str()));
    for (unsigned c = 0; c < cnt; c++) {
        if (c >= bits.size()) {   // missing components read (0, 0, 0, 1)
            if (load) {
                bool fl = type == "FLOAT" || type == "UNORM" || type == "SNORM" || type == "USCALED" || type == "SSCALED";
                line(fmt("  %s = %s;", V(in.vdata + c).c_str(), c == 3 ? (fl ? "0x3f800000u" : "1u") : "0u"));
            }
            continue;
        }
        unsigned b = bits[c], p = pos(c);
        std::string byte = fmt("(a_ + %uu)", p / 8);
        unsigned sh = p % 32;   // within the dword holding the component (element dword-aligned for packed and 32-bit formats)
        std::string shift = b == 32 ? "0u" : (packed ? fmt("%uu", sh) : fmt("((%s & 3u) * 8u)", byte.c_str()));
        std::string mask = b == 32 ? "0xffffffffu" : fmt("0x%xu", (1u << b) - 1);
        std::string src = packed ? "a_" : byte;
        if (load) {
            std::string raw = fmt("((%s >> %s) & %s)", dw(src).c_str(), shift.c_str(), mask.c_str());
            std::string sx = b == 32 ? fmt("int(%s)", raw.c_str()) : fmt("(int(%s << %uu) >> %u)", raw.c_str(), 32 - b, 32 - b);
            std::string v;
            if (type == "UINT") v = raw;
            else if (type == "SINT") v = fmt("uint(%s)", sx.c_str());
            else if (type == "FLOAT") v = b == 32 ? raw : fmt("asuint(f16tof32(%s))", raw.c_str());
            else if (type == "UNORM") v = fmt("asuint(ndiv(float(%s), %.1f))", raw.c_str(), double((1ull << b) - 1));
            else if (type == "SNORM") v = fmt("asuint(max(ndiv(float(%s), %.1f), -1.0))", sx.c_str(), double((1ull << (b - 1)) - 1));
            else if (type == "USCALED") v = fmt("asuint(float(%s))", raw.c_str());
            else if (type == "SSCALED") v = fmt("asuint(float(%s))", sx.c_str());
            else return fail(fmt("%s: buffer format %s not supported", in.name, fname));
            line(fmt("  %s = %s;", V(in.vdata + c).c_str(), v.c_str()));
        } else {
            std::string x = V(in.vdata + c), v;
            if (type == "UINT" || type == "SINT") v = fmt("(%s & %s)", x.c_str(), mask.c_str());
            else if (type == "FLOAT") v = b == 32 ? x : fmt("(f32tof16(asfloat(%s)) & 0xffffu)", x.c_str());
            else if (type == "UNORM") v = fmt("(uint(round(saturate(asfloat(%s)) * %.1f)) & %s)", x.c_str(), double((1ull << b) - 1), mask.c_str());
            else if (type == "SNORM") v = fmt("(uint(int(round(clamp(asfloat(%s), -1.0, 1.0) * %.1f))) & %s)", x.c_str(), double((1ull << (b - 1)) - 1), mask.c_str());
            else return fail(fmt("%s: buffer format %s not supported", in.name, fname));
            if (b == 32) {
                if (R.kind == ResKind::RawBuf) line(fmt("  %s.Store((%s) & ~3u, %s);", RN.c_str(), src.c_str(), v.c_str()));
                else line(fmt("  %s = %s;", word(fmt("((%s) & ~3u)", src.c_str())).c_str(), v.c_str()));
            } else {
                // Sub-dword component: clear and set its bits atomically (neighbouring lanes may share the dword).
                line(fmt("  { uint s_ = %s; uint o_;", shift.c_str()));
                if (R.kind == ResKind::RawBuf) {
                    line(fmt("    %s.InterlockedAnd((%s) & ~3u, ~(%s << s_), o_);", RN.c_str(), src.c_str(), mask.c_str()));
                    line(fmt("    %s.InterlockedOr((%s) & ~3u, %s << s_, o_); }", RN.c_str(), src.c_str(), v.c_str()));
                } else {
                    std::string t = word(fmt("((%s) & ~3u)", src.c_str()));
                    line(fmt("    InterlockedAnd(%s, ~(%s << s_), o_);", t.c_str(), mask.c_str()));
                    line(fmt("    InterlockedOr(%s, %s << s_, o_); }", t.c_str(), v.c_str()));
                }
            }
        }
    }
    line("}");
    return true;
}
bool Translator::emit_mubuf(const Instr& in, size_t idx) {
    std::string n = in.name;
    if (n == "buffer_gl0_inv" || n == "buffer_gl1_inv") return true;   // cache invalidation: PC memory model handles it
    int r = res_of.count(idx) ? res_of[idx] : -1;
    if (r < 0 && dyn_mubuf.count(idx) && starts(n, "buffer_load_format_")) {
        // Typed load through a run-time descriptor: format = dword 3 [18:12], decoded by fmt_comp().
        if (in.d16) return fail(n + ": D16 through a run-time descriptor not supported");
        uses_gm = true;
        uses_fmt_comp = true;
        unsigned cnt = ends(n, "_xyzw") ? 4 : ends(n, "_xyz") ? 3 : ends(n, "_xy") ? 2 : 1;
        unsigned va = in.vaddr;
        std::string vidx = in.idxen ? V(va) : "0u";
        std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
        std::string so = rd(in.soffset, in);
        const std::string d0 = S(in.srsrc), d1 = S(in.srsrc + 1), d3 = S(in.srsrc + 3);
        line(fmt("if (kexec) { uint off_ = %s + %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), so.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
        line(fmt("  uint lo_ = %s + off_, hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u), f_ = (%s >> 12) & 0x7fu;", d0.c_str(), d1.c_str(), d0.c_str(), d3.c_str()));
        for (unsigned k = 0; k < cnt; k++) line(fmt("  %s = fmt_comp(lo_, hi_, f_, %uu);", V(in.vdata + k).c_str(), k));
        line("}");
        return true;
    }
    if (r < 0 && dyn_mubuf.count(idx) && starts(n, "buffer_store_format_")) {
        // Typed store through a run-time descriptor: each component is encoded for the format in
        // dword 3 [18:12] and written to global memory (fmt_put()).
        if (in.d16) return fail(n + ": D16 through a run-time descriptor not supported");
        uses_gm = true;
        uses_fmt_comp = true;
        uses_fmt_put = true;
        unsigned cnt = ends(n, "_xyzw") ? 4 : ends(n, "_xyz") ? 3 : ends(n, "_xy") ? 2 : 1;
        unsigned va = in.vaddr;
        std::string vidx = in.idxen ? V(va) : "0u";
        std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
        std::string so = rd(in.soffset, in);
        const std::string d0 = S(in.srsrc), d1 = S(in.srsrc + 1), d3 = S(in.srsrc + 3);
        line(fmt("if (kexec) { uint off_ = %s + %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), so.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
        line(fmt("  uint lo_ = %s + off_, hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u), f_ = (%s >> 12) & 0x7fu;", d0.c_str(), d1.c_str(), d0.c_str(), d3.c_str()));
        for (unsigned k = 0; k < cnt; k++) line(fmt("  fmt_put(lo_, hi_, f_, %uu, %s);", k, V(in.vdata + k).c_str()));
        line("}");
        return true;
    }
    if (r < 0 && dyn_mubuf.count(idx) && starts(n, "tbuffer_load_format_")) {
        // The same with the format the instruction names (the descriptor gives the address and
        // the stride): only the formats fmt_comp() decodes, components of one size of 8, 16 or 32 bits.
        if (in.d16) return fail(n + ": D16 through a run-time descriptor not supported");
        const char* fname = nullptr;
        for (auto& f : kBufFmts) if (f.id == in.format) fname = f.name;
        bool ok = fname != nullptr;
        if (ok) {
            std::string nm = fname;
            unsigned first = 0, comps = 0;
            for (size_t p0 = 0; p0 < nm.size() && isdigit((unsigned char)nm[p0]);) {
                size_t q = nm.find('_', p0);
                unsigned z = (unsigned)atoi(nm.substr(p0, q - p0).c_str());
                if (!comps) first = z;
                ok = ok && z == first && (z == 8 || z == 16 || z == 32);
                comps++;
                p0 = q == std::string::npos ? nm.size() : q + 1;
            }
            ok = ok && comps > 0;
        }
        if (!ok) return fail(fmt("%s at 0x%x: buffer format %s (%u) through a run-time descriptor not supported", in.name, in.offset, fname ? fname : "?", in.format));
        uses_gm = true;
        uses_fmt_comp = true;
        unsigned cnt = ends(n, "_xyzw") ? 4 : ends(n, "_xyz") ? 3 : ends(n, "_xy") ? 2 : 1;
        unsigned va = in.vaddr;
        std::string vidx = in.idxen ? V(va) : "0u";
        std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
        std::string so = rd(in.soffset, in);
        const std::string d0 = S(in.srsrc), d1 = S(in.srsrc + 1);
        line(fmt("if (kexec) { uint off_ = %s + %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), so.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
        line(fmt("  uint lo_ = %s + off_, hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u);", d0.c_str(), d1.c_str(), d0.c_str()));
        for (unsigned k = 0; k < cnt; k++) line(fmt("  %s = fmt_comp(lo_, hi_, %uu, %uu);", V(in.vdata + k).c_str(), (unsigned)in.format, k));
        line("}");
        return true;
    }
    if (r < 0 && dyn_mubuf.count(idx) && dyn_small_op(n)) {
        // Byte/short through a run-time descriptor: masked atomic stores, shifted loads.
        uses_gm = true;
        unsigned op = dyn_small_op(n);
        unsigned va = in.vaddr;
        std::string vidx = in.idxen ? V(va) : "0u";
        std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
        std::string so = rd(in.soffset, in);
        const std::string d0 = S(in.srsrc), d1 = S(in.srsrc + 1);
        line(fmt("if (kexec) { uint off_ = %s + %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), so.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
        line(fmt("  uint lo_ = %s + off_, hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u);", d0.c_str(), d1.c_str(), d0.c_str()));
        uint32_t mask = (op == 1 || op == 3 || op == 4) ? 0xffu : 0xffffu;
        if (op <= 2) line(fmt("  gm_store_bits(lo_, hi_, %s & 0x%xu, 0x%xu);", V(in.vdata).c_str(), mask, mask));
        else {
            line(fmt("  uint w_ = gm_load(lo_ & ~3u, hi_) >> ((lo_ & 3u) * 8u);"));
            if (op == 3 || op == 5) line(fmt("  %s = w_ & 0x%xu;", V(in.vdata).c_str(), mask));
            else line(fmt("  %s = uint(int(w_ << %uu) >> %uu);", V(in.vdata).c_str(), mask == 0xff ? 24u : 16u, mask == 0xff ? 24u : 16u));
        }
        line("}");
        return true;
    }
    if (r < 0 && dyn_mubuf.count(idx) && dyn_atomic_op(n)) {
        // 32-bit atomic through a run-time descriptor: global memory at base + offset.
        uses_gm = true;
        uses_gm_atomic = true;
        unsigned va = in.vaddr;
        std::string vidx = in.idxen ? V(va) : "0u";
        std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
        std::string so = rd(in.soffset, in);
        const std::string d0 = S(in.srsrc), d1 = S(in.srsrc + 1);
        line(fmt("if (kexec) { uint off_ = %s + %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), so.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
        line(fmt("  uint lo_ = %s + off_, hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u);", d0.c_str(), d1.c_str(), d0.c_str()));
        line(fmt("  uint o_ = gm_atomic(lo_, hi_, %uu, %s);", dyn_atomic_op(n), V(in.vdata).c_str()));
        if (in.glc) line(fmt("  %s = o_;", V(in.vdata).c_str()));
        line("}");
        return true;
    }
    if (r < 0 && dyn_mubuf.count(idx)) {
        // Descriptor words in SGPRs: base [47:0], stride [61:48]; global memory at that address.
        uses_gm = true;
        unsigned cnt = ends(n, "x4") ? 4 : ends(n, "x3") ? 3 : ends(n, "x2") ? 2 : 1;
        unsigned va = in.vaddr;
        std::string vidx = in.idxen ? V(va) : "0u";
        std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
        std::string so = rd(in.soffset, in);
        const std::string d0 = S(in.srsrc), d1 = S(in.srsrc + 1);
        if (opt.raw_tid_buffers && cnt == 1) {
            line(fmt("if (kexec) { uint off_ = %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
            line(fmt("  if ((%s & 0x800000u) != 0u) off_ = (off_ & ~3u) * WAVE + LG * 4u + (off_ & 3u);   // ADD_TID_ENABLE: swizzled per lane", S(in.srsrc + 3).c_str()));
            line(fmt("  off_ += %s;", so.c_str()));
        } else
            line(fmt("if (kexec) { uint off_ = %s + %s + %uu + %s * ((%s >> 16) & 0x3fffu);", voff.c_str(), so.c_str(), uint32_t(in.offset_imm), vidx.c_str(), d1.c_str()));
        line(fmt("  uint lo_ = %s + off_, hi_ = (%s & 0xffffu) + (lo_ < %s ? 1u : 0u);", d0.c_str(), d1.c_str(), d0.c_str()));
        bool store = has(n, "_store_");
        if (!store) { std::vector<std::string> d; for (unsigned k = 0; k < cnt; k++) d.push_back(V(in.vdata + k)); gm_read_n(d); }
        else
            for (unsigned k = 0; k < cnt; k++) {
                std::string a = fmt("lo_ + %uu, hi_ + (lo_ + %uu < lo_ ? 1u : 0u)", 4 * k, 4 * k);
                line(fmt("  gm_store(%s, %s);", a.c_str(), V(in.vdata + k).c_str()));
            }
        line("}");
        return true;
    }
    if (r < 0) return false;
    Res& R = res[r];
    const std::string RN = rn(idx, r);
    unsigned va = in.vaddr;
    std::string vidx = in.idxen ? V(va) : "0u";
    std::string voff = in.offen ? V(in.idxen ? va + 1 : va) : "0u";
    std::string so = rd(in.soffset, in);
    uint32_t stride = R.kind == ResKind::StructBuf ? R.stride : ((R.root_hi_or >> 16) & 0x3fff);
    // Constant buffers indexed per lane: root CBVs get the stride from the bits the shader ORs into
    // the descriptor; table CBV descriptors index 16-byte rows (the compiler relies on that).
    if (R.kind == ResKind::CBuffer && in.idxen) stride = R.root_hi_or ? (R.root_hi_or >> 16) & 0x3fff : 16;
    std::string addr = fmt("(%s + %s + %uu)", voff.c_str(), so.c_str(), uint32_t(in.offset_imm) + R.addr_off);
    if (in.idxen && R.kind != ResKind::TypedBuf) addr = fmt("(%s + %s * %uu)", addr.c_str(), vidx.c_str(), stride);
    bool store = has(n, "_store_");
    bool atomic = has(n, "_atomic_");
    if (store || atomic) R.uav = true;
    auto word = [&](const std::string& a) -> std::string {   // lvalue/rvalue for the dword at byte address a
        if (R.kind == ResKind::StructBuf) return fmt("%s[(%s) / %uu].w[((%s) %% %uu) >> 2]", RN.c_str(), a.c_str(), R.stride, a.c_str(), R.stride);
        return "";
    };
    if (R.kind == ResKind::TypedBuf) {
        unsigned cnt = ends(n, "_xyzw") ? 4 : ends(n, "_xyz") ? 3 : ends(n, "_xy") ? 2 : 1;
        R.comps = std::max(R.comps == 4 && !R.is_uint ? 0u : R.comps, cnt);
        std::string el = in.idxen ? vidx : "0u";
        if (in.offen) el = fmt("(%s + %s / 4u)", el.c_str(), voff.c_str());
        if (atomic) {
            R.is_uint = true;
            R.scalar = true;
        }
        if (starts(n, "buffer_load_format") || starts(n, "tbuffer_load_format")) {
            if (R.scalar) line(fmt("if (kexec) { uint4 t_ = uint4(%s[%s], 0u, 0u, 1u);", RN.c_str(), el.c_str()));
            else line(fmt("if (kexec) { %s t_ = %s[%s];", R.is_uint ? "uint4" : "float4", RN.c_str(), el.c_str()));
            static const char* c4 = "xyzw";
            for (unsigned k = 0; k < cnt; k++) line(fmt("  %s = %s(t_.%c);", V(in.vdata + k).c_str(), R.is_uint ? "uint" : "asuint", c4[k]));
            line("}");
            return true;
        }
        if (starts(n, "buffer_store_format") || starts(n, "tbuffer_store_format")) {
            std::string v;
            for (unsigned k = 0; k < 4; k++) v += (k ? ", " : "") + (k < cnt ? (R.is_uint ? V(in.vdata + k) : "asfloat(" + V(in.vdata + k) + ")") : std::string(R.is_uint ? "0u" : "0.0"));
            if (R.scalar) line(fmt("if (kexec) %s[%s] = %s;", RN.c_str(), el.c_str(), V(in.vdata).c_str()));
            else line(fmt("if (kexec) %s[%s] = %s4(%s);", RN.c_str(), el.c_str(), R.is_uint ? "uint" : "float", v.c_str()));
            return true;
        }
        if (atomic) {
            std::string op = n.substr(14);
            bool rtn = in.glc;
            static const char* m[][2] = {{"add", "InterlockedAdd"}, {"umin", "InterlockedMin"}, {"umax", "InterlockedMax"}, {"smin", "InterlockedMin"},
                                         {"smax", "InterlockedMax"}, {"and", "InterlockedAnd"}, {"or", "InterlockedOr"}, {"xor", "InterlockedXor"},
                                         {"swap", "InterlockedExchange"}};
            for (auto& p : m)
                if (op == p[0]) {
                    if (rtn) line(fmt("if (kexec) %s(%s[%s], %s, %s);", p[1], RN.c_str(), el.c_str(), V(in.vdata).c_str(), V(in.vdata).c_str()));
                    else line(fmt("if (kexec) %s(%s[%s], %s);", p[1], RN.c_str(), el.c_str(), V(in.vdata).c_str()));
                    return true;
                }
            return fail(n + " on a typed buffer not supported");
        }
        return fail(n + " not supported");
    }
    if (R.kind == ResKind::Counter) {
        std::string B = res[R.counter_of].name;
        res[R.counter_of].uav = true;
        if (n == "buffer_atomic_add" || n == "buffer_atomic_sub") {
            bool add = n == "buffer_atomic_add";
            line(fmt("if (kexec) { uint o_ = 0u; for (uint k_ = 0u; k_ < %s; k_++) { uint t_ = %s.%s(); if (k_ == 0u) o_ = t_%s; }", V(in.vdata).c_str(), B.c_str(),
                     add ? "IncrementCounter" : "DecrementCounter", add ? "" : " + 1u"));
            if (in.glc) line(fmt("  %s = o_;", V(in.vdata).c_str()));
            line("}");
            return true;
        }
        if (starts(n, "buffer_load_dword")) return fail(n + ": reading a buffer counter not supported");
        return fail(n + " on a buffer counter not supported");
    }
    if (R.kind == ResKind::CBuffer && starts(n, "buffer_load_dword")) {
        unsigned cnt = has(n, "x4") ? 4 : has(n, "x3") ? 3 : has(n, "x2") ? 2 : 1;
        if (R.dyn) return fail(n + ": constant-buffer arrays not supported");
        line(fmt("if (kexec) { uint a_ = %s;", addr.c_str()));
        for (unsigned k = 0; k < cnt; k++) line(fmt("  %s = cbload(%s_d, a_ + %uu);", V(in.vdata + k).c_str(), R.name.c_str(), 4 * k));
        line("}");
        return true;
    }
    if (R.kind != ResKind::RawBuf && R.kind != ResKind::StructBuf) return fail(fmt("%s at 0x%x: not a buffer resource", in.name, in.offset));
    if (in.lds) return fail(n + " with LDS not supported");
    if (starts(n, "tbuffer_load_format_") || starts(n, "tbuffer_store_format_")) return emit_tbuffer_raw(in, R, RN, addr, word);
    if (starts(n, "buffer_load_dword") || starts(n, "buffer_load_ubyte") || starts(n, "buffer_load_sbyte") || starts(n, "buffer_load_ushort") ||
        starts(n, "buffer_load_sshort") || starts(n, "buffer_load_short_d16") || starts(n, "buffer_load_ubyte_d16") || starts(n, "buffer_load_sbyte_d16")) {
        unsigned cnt = has(n, "x4") ? 4 : has(n, "x3") ? 3 : has(n, "x2") ? 2 : 1;
        line(fmt("if (kexec) { uint a_ = %s;", addr.c_str()));
        if (starts(n, "buffer_load_dword")) {
            for (unsigned k = 0; k < cnt; k++) {
                std::string a = fmt("a_ + %uu", 4 * k);
                std::string v = R.kind == ResKind::RawBuf ? fmt("%s.Load((%s) & ~3u)", RN.c_str(), a.c_str()) : word(a);
                line(fmt("  %s = %s;", V(in.vdata + k).c_str(), v.c_str()));
            }
        } else {
            std::string w = R.kind == ResKind::RawBuf ? fmt("%s.Load(a_ & ~3u)", RN.c_str()) : word("a_ & ~3u");
            std::string v = fmt("(%s >> ((a_ & 3u) * 8u))", w.c_str());
            if (has(n, "ubyte")) v = "(" + v + " & 0xffu)";
            else if (has(n, "sbyte")) v = "uint(int(" + v + " << 24) >> 24)";
            else if (has(n, "ushort") || has(n, "short_d16")) v = "(" + v + " & 0xffffu)";
            else if (has(n, "sshort")) v = "uint(int(" + v + " << 16) >> 16)";
            if (has(n, "_d16_hi")) line(fmt("  %s = (%s & 0xffffu) | (%s << 16);", V(in.vdata).c_str(), V(in.vdata).c_str(), v.c_str()));
            else if (has(n, "_d16")) line(fmt("  %s = (%s & 0xffff0000u) | (%s & 0xffffu);", V(in.vdata).c_str(), V(in.vdata).c_str(), v.c_str()));
            else line(fmt("  %s = %s;", V(in.vdata).c_str(), v.c_str()));
        }
        line("}");
        return true;
    }
    if (starts(n, "buffer_store_dword") || starts(n, "buffer_store_byte") || starts(n, "buffer_store_short")) {
        unsigned cnt = has(n, "x4") ? 4 : has(n, "x3") ? 3 : has(n, "x2") ? 2 : 1;
        line(fmt("if (kexec) { uint a_ = %s;", addr.c_str()));
        if (starts(n, "buffer_store_dword")) {
            for (unsigned k = 0; k < cnt; k++) {
                std::string a = fmt("a_ + %uu", 4 * k);
                if (R.kind == ResKind::RawBuf) line(fmt("  %s.Store((%s) & ~3u, %s);", RN.c_str(), a.c_str(), V(in.vdata + k).c_str()));
                else line(fmt("  %s = %s;", word(a).c_str(), V(in.vdata + k).c_str()));
            }
        } else {
            bool byte = has(n, "byte");
            std::string val = V(in.vdata);
            if (has(n, "_d16_hi")) val = "(" + val + " >> 16)";
            std::string mask = byte ? "0xffu" : "0xffffu";
            std::string target = R.kind == ResKind::RawBuf ? "" : word("a_ & ~3u");
            // Sub-dword stores: atomic read-modify-write of the containing dword.
            if (R.kind == ResKind::RawBuf) {
                line(fmt("  uint sh_ = (a_ & 3u) * 8u; uint o_; %s.InterlockedAnd(a_ & ~3u, ~(%s << sh_), o_); %s.InterlockedOr(a_ & ~3u, (%s & %s) << sh_, o_);",
                         RN.c_str(), mask.c_str(), RN.c_str(), val.c_str(), mask.c_str()));
            } else {
                line(fmt("  uint sh_ = (a_ & 3u) * 8u; InterlockedAnd(%s, ~(%s << sh_)); InterlockedOr(%s, (%s & %s) << sh_);", target.c_str(), mask.c_str(),
                         target.c_str(), val.c_str(), mask.c_str()));
            }
        }
        line("}");
        return true;
    }
    if (atomic) {
        std::string op = n.substr(14);
        bool x2 = ends(op, "_x2");
        if (x2) return fail(n + " (64-bit buffer atomic) not supported yet");
        bool rtn = in.glc;
        static const char* m[][2] = {{"add", "InterlockedAdd"}, {"umin", "InterlockedMin"}, {"umax", "InterlockedMax"}, {"smin", "InterlockedMin"},
                                     {"smax", "InterlockedMax"}, {"and", "InterlockedAnd"}, {"or", "InterlockedOr"}, {"xor", "InterlockedXor"},
                                     {"swap", "InterlockedExchange"}, {"cmpswap", "InterlockedCompareExchange"}, {"sub", "InterlockedAdd"}};
        for (auto& p : m) {
            if (op != p[0]) continue;
            std::string val = V(in.vdata);
            if (op == "sub") val = "(0u - " + val + ")";
            bool sgn = op == "smin" || op == "smax";
            line(fmt("if (kexec) { uint a_ = %s; %s o_;", addr.c_str(), sgn ? "int" : "uint"));
            std::string tv = sgn ? "int(" + val + ")" : val;
            if (op == "cmpswap") {
                // vdata = new value, vdata+1 = compare value.
                if (R.kind == ResKind::RawBuf) line(fmt("  %s.InterlockedCompareExchange(a_, %s, %s, o_);", RN.c_str(), V(in.vdata + 1).c_str(), V(in.vdata).c_str()));
                else line(fmt("  InterlockedCompareExchange(%s, %s, %s, o_);", word("a_").c_str(), V(in.vdata + 1).c_str(), V(in.vdata).c_str()));
            } else if (R.kind == ResKind::RawBuf) {
                line(fmt("  %s.%s(a_, %s, o_);", RN.c_str(), p[1], tv.c_str()));
            } else if (sgn) {
                // A structured buffer's elements are unsigned here, and so is its InterlockedMin/Max:
                // the signed forms are a compare-and-swap that gives up when the value no longer wins.
                const std::string t = word("a_");
                line(fmt("  uint c_ = %s, w_ = %s;", t.c_str(), val.c_str()));
                line(fmt("  [loop] for (uint i_ = 0u; i_ < 24u; i_++) { if (!(int(w_) %s int(c_))) break; uint p_; InterlockedCompareExchange(%s, c_, w_, p_); if (p_ == c_) break; c_ = p_; }",
                         op == "smin" ? "<" : ">", t.c_str()));
                line("  o_ = int(c_);");
            } else {
                line(fmt("  %s(%s, %s, o_);", p[1], word("a_").c_str(), tv.c_str()));
            }
            if (rtn) line(fmt("  %s = uint(o_);", V(in.vdata).c_str()));
            line("}");
            return true;
        }
        return fail(n + " not supported");
    }
    return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
}

// ---- images ------------------------------------------------------------------------------
// Image load/store through the global heap: descriptor dword 0 = heap index, dword 1 = view kind
// (bit 0 UAV, bits 7:4 class 0 float / 1 uint / 2 sint). Arrays HU<dim><cls> (UAVs) and HS<dim><cls>
// (SRVs) cover the whole heap (spaces 910+). 2D, 2D array and 3D; no A16/D16.
bool Translator::emit_heap_image(const Instr& in, size_t idx) {
    std::string n = in.name;
    auto areg = [&](unsigned k) -> std::string {   // A16: two 16-bit coordinates per register
        if (!in.a16) return V(in.nsa && k ? in.addr_extra[k - 1] : in.vaddr + k);
        unsigned r = k / 2;
        std::string v = V(in.nsa && r ? in.addr_extra[r - 1] : in.vaddr + r);
        return fmt("((%s >> %uu) & 0xffffu)", v.c_str(), 16 * (k & 1));
    };   // NSA: scattered address registers
    const unsigned dim = in.dim;
    if (dim != 0 && dim != 1 && dim != 5 && dim != 2) return fail(fmt("%s at 0x%x: dim %u through descriptor memory not supported", in.name, in.offset, dim));
    heap_dims.insert(dim);
    const char* tag = dim == 0 ? "1" : dim == 1 ? "2" : dim == 5 ? "2A" : "3";
    unsigned nc = dim == 0 ? 1 : dim == 1 ? 2 : 3;
    std::string c = nc == 1 ? fmt("uint(%s)", areg(0).c_str())
                  : nc == 2 ? fmt("uint2(%s, %s)", areg(0).c_str(), areg(1).c_str())
                            : fmt("uint3(%s, %s, %s)", areg(0).c_str(), areg(1).c_str(), areg(2).c_str());
    std::string mip = n == "image_load_mip" ? areg(nc) : std::string("0u");
    const std::string hi = S(in.srsrc), hk = S(in.srsrc + 1);   // layer payload: heap index, view kind
    if (n == "image_store") {
        std::string fv, uv, iv;
        unsigned o = 0;
        for (unsigned k = 0; k < 4; k++) {
            std::string e = "0u", fe;
            if (in.dmask >> k & 1) {
                if (in.d16) {   // 16-bit components, two per register (floats as f16)
                    std::string r = V(in.vdata + o / 2);
                    e = o % 2 ? "(" + r + " >> 16)" : "(" + r + " & 0xffffu)";
                    fe = "f16tof32(" + e + ")";
                    o++;
                } else e = V(in.vdata + o++);
            }
            if (fe.empty()) fe = "asfloat(" + e + ")";
            fv += (k ? ", " : "") + fe;
            uv += (k ? ", " : "") + e;
            iv += (k ? ", " : "") + ("int(" + e + ")");
        }
        line(fmt("if (kexec) { uint hix_ = %s, cls_ = (%s >> 4) & 15u;", hi.c_str(), hk.c_str()));
        line(fmt("  if (cls_ == 1u) HU%sU[NonUniformResourceIndex(hix_)][%s] = uint4(%s);", tag, c.c_str(), uv.c_str()));
        line(fmt("  else if (cls_ == 2u) HU%sI[NonUniformResourceIndex(hix_)][%s] = int4(%s);", tag, c.c_str(), iv.c_str()));
        line(fmt("  else HU%sF[NonUniformResourceIndex(hix_)][%s] = float4(%s); }", tag, c.c_str(), fv.c_str()));
        return true;
    }
    // image_load / image_load_mip: UAVs read without a level, SRVs with one
    std::string lc = nc == 1 ? fmt("int2(%s, int(%s))", c.c_str(), mip.c_str())
                   : nc == 2 ? fmt("int3(%s, int(%s))", c.c_str(), mip.c_str()) : fmt("int4(%s, int(%s))", c.c_str(), mip.c_str());
    line(fmt("if (kexec) { uint hix_ = %s, hk_ = %s, cls_ = (hk_ >> 4) & 15u; uint4 t_;", hi.c_str(), hk.c_str()));
    line("  if (hk_ & 1u) {");
    line(fmt("    if (cls_ == 1u) t_ = HU%sU[NonUniformResourceIndex(hix_)][%s];", tag, c.c_str()));
    line(fmt("    else if (cls_ == 2u) t_ = uint4(HU%sI[NonUniformResourceIndex(hix_)][%s]);", tag, c.c_str()));
    line(fmt("    else t_ = asuint(HU%sF[NonUniformResourceIndex(hix_)][%s]);", tag, c.c_str()));
    line("  } else {");
    line(fmt("    if (cls_ == 1u) t_ = HS%sU[NonUniformResourceIndex(hix_)].Load(%s);", tag, lc.c_str()));
    line(fmt("    else if (cls_ == 2u) t_ = uint4(HS%sI[NonUniformResourceIndex(hix_)].Load(%s));", tag, lc.c_str()));
    line(fmt("    else t_ = asuint(HS%sF[NonUniformResourceIndex(hix_)].Load(%s));", tag, lc.c_str()));
    line("  }");
    unsigned o = 0;
    static const char* comp = "xyzw";
    if (in.d16) {
        // 16-bit results, two per register: float views as f16 (the class is known at run time).
        std::vector<std::string> cs;
        for (unsigned k = 0; k < 4; k++)
            if (in.dmask >> k & 1) cs.push_back(fmt("(cls_ == 0u ? f32tof16(asfloat(t_.%c)) : (t_.%c & 0xffffu))", comp[k], comp[k]));
        for (size_t i = 0; i < cs.size(); i += 2)
            line(fmt("  %s = %s%s;", V(in.vdata + unsigned(i / 2)).c_str(), cs[i].c_str(), i + 1 < cs.size() ? (" | (" + cs[i + 1] + " << 16)").c_str() : ""));
        if (in.tfe) line(fmt("  %s = 0u;", V(in.vdata + unsigned((cs.size() + 1) / 2)).c_str()));
        line("}");
        return true;
    }
    for (unsigned k = 0; k < 4; k++)
        if (in.dmask >> k & 1) line(fmt("  %s = t_.%c;", V(in.vdata + o++).c_str(), comp[k]));
    line("}");
    return true;
}

bool Translator::emit_mimg(const Instr& in, size_t idx) {
    std::string n = in.name;
    if (has(n, "bvh")) {
        if (!opt.global_memory && !opt.global_memory_test) {
            // Without global memory every node test misses: box nodes return no children, so
            // traversal ends at the root and ray queries report a miss.
            for (unsigned k = 0; k < 4; k++) write_v(in.vdata + k, "0xffffffffu");
            uses_inert_bvh = true;
            return true;
        }
        if (in.a16) return fail(n + " with 16-bit ray directions not supported yet");
        // Address operands: node pointer (1 or 2 VGPRs), ray extent, origin xyz, direction xyz,
        // inverse direction xyz. Descriptor: base address [39:0] in 256-byte units, box sorting [63].
        auto ar = [&](unsigned k) { return in.nsa ? V(k == 0 ? in.vaddr : in.addr_extra[k - 1]) : V(in.vaddr + k); };
        bool b64 = has(n, "bvh64");
        unsigned k0 = b64 ? 2 : 1;
        uses_gm = true;
        uses_bvh = true;
        std::string base = fmt("((uint64_t(%s & 0xffu) << 32 | uint64_t(%s)) << 8)", S(in.srsrc + 1).c_str(), S(in.srsrc).c_str());
        std::string ptr = b64 ? fmt("(uint64_t(%s) << 32 | uint64_t(%s))", ar(1).c_str(), ar(0).c_str()) : fmt("uint64_t(%s)", ar(0).c_str());
        line(fmt("if (kexec) { uint4 r_ = bvh_isect(%s, %s, asfloat(%s), float3(asfloat(%s), asfloat(%s), asfloat(%s)), float3(asfloat(%s), asfloat(%s), asfloat(%s)), "
                 "float3(asfloat(%s), asfloat(%s), asfloat(%s)), (%s >> 31) != 0u);",
                 base.c_str(), ptr.c_str(), ar(k0).c_str(), ar(k0 + 1).c_str(), ar(k0 + 2).c_str(), ar(k0 + 3).c_str(), ar(k0 + 4).c_str(), ar(k0 + 5).c_str(),
                 ar(k0 + 6).c_str(), ar(k0 + 7).c_str(), ar(k0 + 8).c_str(), ar(k0 + 9).c_str(), S(in.srsrc + 1).c_str()));
        for (unsigned k = 0; k < 4; k++) line(fmt("  %s = r_[%u];", V(in.vdata + k).c_str(), k));
        if (opt.raw_stats) line("  dbg_add(10u); if (r_[0] != 0xffffffffu) dbg_add(11u);");
        line("}");
        return true;
    }
    int r = res_of.count(idx) ? res_of[idx] : -1;
    const bool dyn = r < 0 && dyn_mimg.count(idx);
    if (dyn && (n == "image_load" || n == "image_load_mip" || n == "image_store")) return emit_heap_image(in, idx);
    if (r < 0 && !dyn) return fail(fmt("%s at 0x%x: image descriptor not known", in.name, in.offset));   // never drop the access silently
    // A descriptor read from descriptor memory at run time: the layer stamps the view's index in
    // the global heap into dword 0, and the texture is that element of the bindless array of the
    // instruction's dimension (float views; see the arrays' declaration). Everything below treats
    // it like a traced texture.
    Res dynRes;
    std::string dynName;
    if (dyn) {
        static const char* const tags[8] = {"1", "2", "3", "C", nullptr, "2A", nullptr, nullptr};
        if (!tags[in.dim]) return fail(fmt("%s at 0x%x: dim %u through descriptor memory not supported", in.name, in.offset, (unsigned)in.dim));
        if (in.dim == 3) uses_heap_cube = true;
        else heap_dims.insert(in.dim);
        dynRes.kind = ResKind::Image;
        dynRes.dim = in.dim;
        dynName = fmt("HS%sF[NonUniformResourceIndex(%s)]", tags[in.dim], S(in.srsrc).c_str());
    }
    Res& R = dyn ? dynRes : res[r];
    const std::string RN = dyn ? dynName : rn(idx, r);
    if (R.kind != ResKind::Image) return fail(fmt("%s at 0x%x: not an image resource", in.name, in.offset));
    if (R.dim < 0) R.dim = in.dim;
    else if (R.dim != in.dim && !(R.dim == 3 && in.dim == 5) && !(R.dim == 5 && in.dim == 3))
        return fail(fmt("%s at 0x%x: image used with two dimensions", in.name, in.offset));
    if (n == "image_get_resinfo") {
        // x = width, y = height (array slices for 1D arrays), z = depth or slices, w = mip levels,
        // all at the mip level in vaddr; written to consecutive VGPRs for the dmask bits.
        if (in.dim == 6 || in.dim == 7) return fail(n + " on a multisampled image not supported");
        std::string mip = V(in.vaddr);
        line("if (kexec) { uint w_ = 1u, h_ = 1u, d_ = 1u, l_ = 1u;");
        if (dyn) {
            // (Dword 1 bit 0: the view is a UAV, which has no level argument.)
            static const char* const tags[8] = {"1", "2", "3", "C", nullptr, "2A", nullptr, nullptr};
            const std::string U = fmt("HU%sF[NonUniformResourceIndex(%s)]", tags[in.dim], S(in.srsrc).c_str());
            const char* dims = in.dim == 0 ? "w_" : in.dim == 1 || in.dim == 3 ? "w_, h_" : "w_, h_, d_";
            if (in.dim == 3) line(fmt("  %s.GetDimensions(%s, %s, l_);", RN.c_str(), mip.c_str(), dims));
            else line(fmt("  if (%s & 1u) %s.GetDimensions(%s); else %s.GetDimensions(%s, %s, l_);", S(in.srsrc + 1).c_str(), U.c_str(), dims, RN.c_str(), mip.c_str(), dims));
            const char* val[4] = {"w_", "h_", "d_", "l_"};
            unsigned o = 0;
            for (unsigned c = 0; c < 4; c++)
                if (in.dmask >> c & 1) line(fmt("  %s = %s;", V(in.vdata + o++).c_str(), val[c]));
            line("}");
            return true;
        }
        bool rw = R.uav;
        switch (in.dim) {
        case 0: line(rw ? fmt("  %s.GetDimensions(w_);", RN.c_str()) : fmt("  %s.GetDimensions(%s, w_, l_);", RN.c_str(), mip.c_str())); break;
        case 1: case 3: line(rw ? fmt("  %s.GetDimensions(w_, h_);", RN.c_str()) : fmt("  %s.GetDimensions(%s, w_, h_, l_);", RN.c_str(), mip.c_str())); break;
        case 2: case 5: line(rw ? fmt("  %s.GetDimensions(w_, h_, d_);", RN.c_str()) : fmt("  %s.GetDimensions(%s, w_, h_, d_, l_);", RN.c_str(), mip.c_str())); break;
        case 4: line(rw ? fmt("  %s.GetDimensions(w_, h_);", RN.c_str()) : fmt("  %s.GetDimensions(%s, w_, h_, l_);", RN.c_str(), mip.c_str())); break;
        default: break;
        }
        const char* val[4] = {"w_", "h_", "d_", "l_"};
        unsigned o = 0;
        for (unsigned c = 0; c < 4; c++)
            if (in.dmask >> c & 1) line(fmt("  %s = %s;", V(in.vdata + o++).c_str(), val[c]));
        line("}");
        return true;
    }
    // Address registers. With A16, coordinates (with LOD and clamp) and each derivative group are
    // 16-bit values packed two per register; offsets, bias and depth reference keep a register each.
    std::vector<std::string> ad;
    auto areg = [&](unsigned k) -> std::string {
        if (in.nsa) return V(k == 0 ? in.vaddr : in.addr_extra[k - 1]);
        return V(in.vaddr + k);
    };
    static const unsigned ncoord[8] = {1, 2, 3, 3, 2, 3, 3, 4};
    static const unsigned nderiv[8] = {1, 2, 3, 2, 1, 2, 2, 2};
    const unsigned dim = in.dim, nc = ncoord[dim];
    unsigned k = 0;
    bool samp = starts(n, "image_sample") || starts(n, "image_gather4") || n == "image_get_lod";
    std::string offs, bias, zc, lod, clampv;
    std::vector<std::string> ddx, ddy, crd;
    if (samp) {
        if (has(n, "_o")) offs = areg(k++);
        if (has(n, "_b")) bias = areg(k++);
        if (has(n, "_c") && !has(n, "_cl") ) zc = areg(k++);
        else if (has(n, "_c_")) zc = areg(k++);
        if (has(n, "_d") || has(n, "_cd")) {
            for (unsigned i = 0; i < nderiv[dim]; i++) ddx.push_back(areg(k++));
            for (unsigned i = 0; i < nderiv[dim]; i++) ddy.push_back(areg(k++));
        }
    }
    if (!in.a16) {
        for (unsigned i = 0; i < nc; i++) crd.push_back(areg(k++));
        if (samp && (has(n, "_l") && !has(n, "_lz"))) lod = areg(k++);
        if (samp && has(n, "_cl")) clampv = areg(k++);
        if (n == "image_load_mip" || n == "image_store_mip") lod = areg(k++);
    } else {
        bool flt = samp;   // sampling takes f16 coordinates; loads and stores take 16-bit integers
        auto half = [&](unsigned reg, unsigned h) {
            std::string r = areg(reg);
            std::string e = h ? "(" + r + " >> 16)" : "(" + r + " & 0xffffu)";
            return flt ? "asuint(f16tof32(" + e + "))" : e;
        };
        if (!bias.empty()) bias = fmt("asuint(f16tof32(%s & 0xffffu))", bias.c_str());
        if (!ddx.empty()) {   // re-read the derivatives as packed halves
            k -= 2 * nderiv[dim];
            unsigned per = (nderiv[dim] + 1) / 2;
            for (unsigned i = 0; i < nderiv[dim]; i++) ddx[i] = half(k + i / 2, i % 2);
            for (unsigned i = 0; i < nderiv[dim]; i++) ddy[i] = half(k + per + i / 2, i % 2);
            k += 2 * per;
        }
        unsigned h = 0;
        for (unsigned i = 0; i < nc; i++, h++) crd.push_back(half(k + h / 2, h % 2));
        if (samp && (has(n, "_l") && !has(n, "_lz"))) { lod = half(k + h / 2, h % 2); h++; }
        if (samp && has(n, "_cl")) { clampv = half(k + h / 2, h % 2); h++; }
        if (n == "image_load_mip" || n == "image_store_mip") { lod = half(k + h / 2, h % 2); h++; }
        k += (h + 1) / 2;
    }

    const char* sw = "xyzw";
    auto fcoord = [&](unsigned cnt) {
        std::string s = cnt > 1 ? fmt("float%u(", cnt) : "(";
        for (unsigned i = 0; i < cnt; i++) s += (i ? ", " : "") + std::string("asfloat(") + crd[i] + ")";
        return s + ")";
    };
    auto icoord = [&](unsigned cnt, const std::string& extra) {
        unsigned tot = cnt + (extra.empty() ? 0 : 1);
        std::string s = tot > 1 ? fmt("uint%u(", tot) : "(";
        for (unsigned i = 0; i < cnt; i++) s += (i ? ", " : "") + crd[i];
        if (!extra.empty()) s += ", " + extra;
        return s + ")";
    };
    // D16: components are 16-bit, packed two per register (floats as f16).
    auto pack16 = [&](const std::string& c) { return R.is_uint ? "(" + c + " & 0xffffu)" : "f16r(" + c + ")"; };
    auto write_result = [&](const std::string& vec4) {
        if (in.d16) {
            line(fmt("if (kexec) { %s t_ = %s;", R.is_uint || R.scalar ? "uint4" : "float4", R.scalar ? ("uint4(" + vec4 + ", 0u, 0u, 1u)").c_str() : vec4.c_str()));
            std::vector<std::string> c;
            for (int b = 0; b < 4; b++)
                if ((in.dmask >> b) & 1) c.push_back(pack16(fmt("t_.%c", sw[b])));
            for (size_t i = 0; i < c.size(); i += 2)
                line(fmt("  %s = %s%s;", V(in.vdata + unsigned(i / 2)).c_str(), c[i].c_str(), i + 1 < c.size() ? (" | (" + c[i + 1] + " << 16)").c_str() : ""));
            if (in.tfe) line(fmt("  %s = 0u;", V(in.vdata + unsigned((c.size() + 1) / 2)).c_str()));
            line("}");
            return;
        }
        if (R.scalar) line(fmt("if (kexec) { uint4 t_ = uint4(%s, 0u, 0u, 1u);", vec4.c_str()));
        else line(fmt("if (kexec) { %s t_ = %s;", R.is_uint ? "uint4" : "float4", vec4.c_str()));
        unsigned o = 0;
        for (int b = 0; b < 4; b++)
            if ((in.dmask >> b) & 1) line(fmt("  %s = %s(t_.%c);", V(in.vdata + o++).c_str(), R.is_uint ? "uint" : "asuint", sw[b]));
        if (in.tfe) line(fmt("  %s = 0u;", V(in.vdata + o).c_str()));
        line("}");
    };
    if (samp) {
        int s = samp_of.count(idx) ? samp_of[idx] : -1;
        std::string dynSS;
        if (s < 0 && dyn_samp.count(idx)) {
            // A sampler read from descriptor memory: dword 0 is its index in the title's sampler
            // heap, seen as plain and as comparison samplers.
            if (zc.empty()) { uses_heap_sampler = true; dynSS = fmt("HSMP[NonUniformResourceIndex(%s)]", S(in.ssamp).c_str()); }
            else { uses_heap_sampler_cmp = true; dynSS = fmt("HSMPC[NonUniformResourceIndex(%s)]", S(in.ssamp).c_str()); }
        } else if (s < 0) return err.empty() ? fail(fmt("%s at 0x%x: sampler not known", in.name, in.offset)) : false;
        if (s >= 0 && !zc.empty()) res[s].cmp_sampler = true;
        std::string tc;
        if (dim == 3) {
            // AMD cube coordinates: face-relative s, t in [1, 2] and face id + 8 * array slice.
            tc = fmt("cube_dir(asfloat(%s), asfloat(%s), asfloat(%s))", crd[0].c_str(), crd[1].c_str(), crd[2].c_str());
            if (R.cube_array) tc = fmt("float4(%s, floor(round(asfloat(%s)) / 8.0))", tc.c_str(), crd[2].c_str());
        } else if (dim == 4) tc = fmt("float2(asfloat(%s), round(asfloat(%s)))", crd[0].c_str(), crd[1].c_str());
        else if (dim == 5 && R.dim == 3) {
            // A cube resource sampled as a 2D array (layer = face + 6 * slice, u, v in [0, 1]): the PC
            // view is a cube, so the face texel becomes a direction again.
            tc = fmt("cube_dir(asfloat(%s) + 1.0, asfloat(%s) + 1.0, float(uint(round(asfloat(%s))) %% 6u))", crd[0].c_str(), crd[1].c_str(), crd[2].c_str());
            if (R.cube_array) tc = fmt("float4(%s, float(uint(round(asfloat(%s))) / 6u))", tc.c_str(), crd[2].c_str());
        } else if (dim == 5) tc = fmt("float3(asfloat(%s), asfloat(%s), round(asfloat(%s)))", crd[0].c_str(), crd[1].c_str(), crd[2].c_str());
        else tc = fcoord(nc);
        std::string gatherOfs;
        if (!offs.empty()) {
            // Texel offsets (signed 6-bit x [5:0], y [13:8]): on 2D textures and 2D arrays sampled at an
            // explicit level they become a UV shift of offset / mip size (exact at level 0).
            bool explicit_lod = has(n, "_lz") || !lod.empty();
            bool flat = dim == 1 || (dim == 5 && R.dim != 3);
            if (flat && has(n, "gather")) {
                // A gather takes its offset in texels as it is (signed 6-bit x and y).
                gatherOfs = fmt(", int2(int(%s << 26) >> 26, int(%s << 18) >> 26)", offs.c_str(), offs.c_str());
            } else {
            // Without an explicit level (pixel stages): the level the sample will take, as the
            // sampler computes it from the unshifted coordinates (a constant shift leaves the
            // derivatives as they are).
            if (!flat || has(n, "gather") || (!explicit_lod && c.kind != ProgramKind::PS)) return fail(n + ": texel offsets not supported yet");
            const std::string ss0 = s >= 0 ? sn(idx, s) : dynSS;
            std::string mip = has(n, "_lz") ? std::string("0u")
                              : !lod.empty() ? fmt("uint(max(round(asfloat(%s)), 0.0))", lod.c_str())
                              : fmt("uint(max(round(%s.CalculateLevelOfDetail(%s, %s%s)), 0.0))", RN.c_str(), ss0.c_str(), tc.c_str(), dim == 1 ? "" : ".xy");
            static std::atomic<unsigned> ofsSerial{0};   // one name per instruction (a resource may be sampled many times)
            std::string ofs = fmt("ofs%u_", ofsSerial.fetch_add(1));
            line(fmt("float2 %s; { uint o_ = %s; int ox_ = int(o_ << 26) >> 26, oy_ = int(o_ << 18) >> 26; uint w_, h_, d_, l_; %s.GetDimensions(%s, w_, h_, %sl_); "
                     "%s = float2(ox_, oy_) / float2(max(w_, 1u), max(h_, 1u)); }",
                     ofs.c_str(), offs.c_str(), RN.c_str(), mip.c_str(), dim == 5 ? "d_, " : "", ofs.c_str()));
            if (dim == 1) tc = fmt("(%s + %s)", tc.c_str(), ofs.c_str());
            else tc = fmt("(%s + float3(%s, 0.0))", tc.c_str(), ofs.c_str());
            }
        }
        std::string call;
        const std::string T = RN;
        const std::string SS = s >= 0 ? sn(idx, s) : dynSS;
        if (n == "image_get_lod") {
            line(fmt("if (kexec) { float l_ = %s.CalculateLevelOfDetailUnclamped(%s, %s); float lc_ = %s.CalculateLevelOfDetail(%s, %s);", T.c_str(), SS.c_str(), tc.c_str(), T.c_str(), SS.c_str(), tc.c_str()));
            unsigned o = 0;
            if (in.dmask & 1) line(fmt("  %s = asuint(lc_);", V(in.vdata + o++).c_str()));
            if (in.dmask & 2) line(fmt("  %s = asuint(l_);", V(in.vdata + o++).c_str()));
            line("}");
            uses_quad = true;
            return true;
        }
        if (starts(n, "image_gather4")) {
            const char* ch = in.dmask == 1 ? "Red" : in.dmask == 2 ? "Green" : in.dmask == 4 ? "Blue" : "Alpha";
            if (zc.empty() && !lod.empty() && (dim == 1 || (dim == 5 && R.dim != 3))) {
                // A gather at a named level (occlusion tests read a depth pyramid this way). HLSL
                // gathers at level 0 only, so the four texels of the footprint at that level are
                // read one by one, in the order a gather returns them. Texels of a float format
                // are sampled at their centres, where a filter gives the texel itself: the
                // sampler's address mode and border colour then hold as on the console (a pyramid
                // is read with a border). Integer formats cannot be sampled so: their texels are
                // loaded, with coordinates past the edge clamped.
                const char cc = in.dmask == 1 ? 'x' : in.dmask == 2 ? 'y' : in.dmask == 4 ? 'z' : 'w';
                const std::string gofs = offs.empty() ? "int2(0, 0)" : fmt("int2(int(%s << 26) >> 26, int(%s << 18) >> 26)", offs.c_str(), offs.c_str());
                const char* dd = dim == 5 ? "d_, " : "";
                line(fmt("if (kexec) { uint w_, h_, d_, l_; %s.GetDimensions(0u, w_, h_, %sl_); uint m_ = min(uint(max(round(asfloat(%s)), 0.0)), max(l_, 1u) - 1u); "
                         "%s.GetDimensions(m_, w_, h_, %sl_);",
                         T.c_str(), dd, lod.c_str(), T.c_str(), dd));
                line(fmt("  float2 s_ = float2(max(w_, 1u), max(h_, 1u)); int2 q_ = int2(floor((%s).xy * s_ - 0.5)) + %s;", tc.c_str(), gofs.c_str()));
                static const char* corner[4] = {"int2(0, 1)", "int2(1, 1)", "int2(1, 0)", "int2(0, 0)"};
                std::string v;
                for (int b = 0; b < 4; b++) {
                    std::string e;
                    if (R.is_uint || R.scalar)
                        e = dim == 5 ? fmt("%s.Load(int4(clamp(q_ + %s, int2(0, 0), int2(s_) - 1), int((%s).z), int(m_))).%c", T.c_str(), corner[b], tc.c_str(), cc)
                                     : fmt("%s.Load(int3(clamp(q_ + %s, int2(0, 0), int2(s_) - 1), int(m_))).%c", T.c_str(), corner[b], cc);
                    else
                        e = dim == 5 ? fmt("%s.SampleLevel(%s, float3((float2(q_ + %s) + 0.5) / s_, (%s).z), float(m_)).%c", T.c_str(), SS.c_str(), corner[b], tc.c_str(), cc)
                                     : fmt("%s.SampleLevel(%s, (float2(q_ + %s) + 0.5) / s_, float(m_)).%c", T.c_str(), SS.c_str(), corner[b], cc);
                    v += (b ? ", " : "") + e;
                }
                line(fmt("  %s t_ = %s(%s);", R.is_uint || R.scalar ? "uint4" : "float4", R.is_uint || R.scalar ? "uint4" : "float4", v.c_str()));
            } else {
            if (!zc.empty()) call = fmt("%s.GatherCmp%s(%s, %s, asfloat(%s)%s)", T.c_str(), ch, SS.c_str(), tc.c_str(), zc.c_str(), gatherOfs.c_str());
            else call = fmt("%s.Gather%s(%s, %s%s)", T.c_str(), ch, SS.c_str(), tc.c_str(), gatherOfs.c_str());   // (HLSL gathers at level 0)
            // Gather returns 4 values regardless of dmask.
            line(fmt("if (kexec) { %s t_ = %s;", R.is_uint ? "uint4" : "float4", call.c_str()));
            }
            if (in.d16)
                for (int b = 0; b < 4; b += 2)
                    line(fmt("  %s = %s | (%s << 16);", V(in.vdata + b / 2).c_str(), pack16(fmt("t_.%c", sw[b])).c_str(), pack16(fmt("t_.%c", sw[b + 1])).c_str()));
            else
                for (int b = 0; b < 4; b++) line(fmt("  %s = %s(t_.%c);", V(in.vdata + b).c_str(), R.is_uint ? "uint" : "asuint", sw[b]));
            line("}");
            return true;
        }
        if (!zc.empty()) {
            std::string ref = fmt("asfloat(%s)", zc.c_str());
            if (has(n, "_lz") || !lod.empty()) call = fmt("%s.SampleCmpLevelZero(%s, %s, %s)", T.c_str(), SS.c_str(), tc.c_str(), ref.c_str());
            else call = fmt("%s.SampleCmp(%s, %s, %s)", T.c_str(), SS.c_str(), tc.c_str(), ref.c_str());
            call = fmt("float4((%s).xxxx)", call.c_str());
        } else if (has(n, "_lz")) call = fmt("%s.SampleLevel(%s, %s, 0)", T.c_str(), SS.c_str(), tc.c_str());
        else if (!lod.empty()) call = fmt("%s.SampleLevel(%s, %s, asfloat(%s))", T.c_str(), SS.c_str(), tc.c_str(), lod.c_str());
        else if (!bias.empty()) { call = fmt("%s.SampleBias(%s, %s, asfloat(%s))", T.c_str(), SS.c_str(), tc.c_str(), bias.c_str()); uses_quad = true; }
        else if (!ddx.empty()) {
            auto vec = [&](std::vector<std::string>& v) {
                std::string s = v.size() > 1 ? fmt("float%zu(", v.size()) : "(";
                for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + std::string("asfloat(") + v[i] + ")";
                return s + ")";
            };
            if (dim == 3 && ddx.size() == 2 && ddy.size() == 2) {
                // A cube's gradients are those of the face coordinates; the PC wants them as
                // gradients of the direction (linear in s and t on one face).
                call = fmt("%s.SampleGrad(%s, %s, cube_grad(asfloat(%s), asfloat(%s), asfloat(%s)), cube_grad(asfloat(%s), asfloat(%s), asfloat(%s)))", T.c_str(), SS.c_str(),
                           tc.c_str(), ddx[0].c_str(), ddx[1].c_str(), crd[2].c_str(), ddy[0].c_str(), ddy[1].c_str(), crd[2].c_str());
            } else
            call = fmt("%s.SampleGrad(%s, %s, %s, %s)", T.c_str(), SS.c_str(), tc.c_str(), vec(ddx).c_str(), vec(ddy).c_str());
        } else { call = fmt("%s.Sample(%s, %s)", T.c_str(), SS.c_str(), tc.c_str()); uses_quad = true; }
        write_result(call);
        return true;
    }
    if (n == "image_load" || n == "image_load_mip") {
        std::string c;
        if ((dim == 6 || dim == 7) && !R.uav && nc >= 2) {
            // Multisampled: the last coordinate is the fragment (sample) index, a separate argument.
            std::string xy = nc - 1 > 1 ? fmt("uint%u(", nc - 1) : "(";
            for (unsigned i = 0; i + 1 < nc; i++) xy += (i ? ", " : "") + crd[i];
            write_result(fmt("%s.Load(%s), %s)", RN.c_str(), xy.c_str(), crd[nc - 1].c_str()));
            return true;
        }
        if (R.uav) c = icoord(nc, "");
        else c = icoord(nc, lod.empty() ? "0u" : lod);
        write_result(fmt("%s.Load(%s)", RN.c_str(), c.c_str()));
        return true;
    }
    if (n == "image_store" || n == "image_store_mip") {
        R.uav = true;
        if (n == "image_store_mip") return fail("image_store_mip not supported");
        std::string v;
        unsigned o = 0;
        for (int b = 0; b < 4; b++) {
            std::string e = "0u";
            if ((in.dmask >> b) & 1) {
                if (in.d16) {
                    std::string r = V(in.vdata + o / 2);
                    e = o % 2 ? "(" + r + " >> 16)" : "(" + r + " & 0xffffu)";
                    if (!R.is_uint) e = "asuint(f16tof32(" + e + "))";
                    o++;
                } else e = V(in.vdata + o++);
            }
            v += (b ? ", " : "") + (R.is_uint ? e : "asfloat(" + e + ")");
        }
        if (R.scalar) line(fmt("if (kexec) %s[%s] = %s;", RN.c_str(), icoord(nc, "").c_str(), in.d16 ? ("(" + V(in.vdata) + " & 0xffffu)").c_str() : V(in.vdata).c_str()));
        else line(fmt("if (kexec) %s[%s] = %s4(%s);", RN.c_str(), icoord(nc, "").c_str(), R.is_uint ? "uint" : "float", v.c_str()));
        return true;
    }
    if (starts(n, "image_atomic_")) {
        R.uav = true;
        R.is_uint = true;
        std::string op = n.substr(13);
        static const char* m[][2] = {{"add", "InterlockedAdd"}, {"umin", "InterlockedMin"}, {"umax", "InterlockedMax"}, {"smin", "InterlockedMin"},
                                     {"smax", "InterlockedMax"}, {"and", "InterlockedAnd"}, {"or", "InterlockedOr"}, {"xor", "InterlockedXor"},
                                     {"swap", "InterlockedExchange"}, {"cmpswap", "InterlockedCompareExchange"}};
        for (auto& p : m) {
            if (op != p[0]) continue;
            std::string tgt = fmt("%s[%s]", RN.c_str(), icoord(nc, "").c_str());
            line("if (kexec) { uint o_;");
            if (op == "cmpswap") line(fmt("  InterlockedCompareExchange(%s, %s, %s, o_);", tgt.c_str(), V(in.vdata + 1).c_str(), V(in.vdata).c_str()));
            else line(fmt("  %s(%s, %s, o_);", p[1], tgt.c_str(), V(in.vdata).c_str()));
            if (in.glc) line(fmt("  %s = o_;", V(in.vdata).c_str()));
            line("}");
            return true;
        }
        return fail(n + " not supported");
    }
    return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
}

// ---- LDS ---------------------------------------------------------------------------------
bool Translator::emit_ds(const Instr& in, size_t idx) {
    std::string n = in.name;
    uses_lds = true;
    std::string a = fmt("(%s + %uu)", V(in.vaddr).c_str(), uint32_t(in.offset_imm) | (uint32_t(in.gds_offset1) << 8));
    if (in.gds) return fail(n + " with GDS not supported");
    if (n == "ds_swizzle_b32") {
        uint32_t off = uint32_t(in.offset_imm) | (uint32_t(in.gds_offset1) << 8);
        std::string v = V(in.vaddr);   // the swizzled value is the ADDR operand (no DATA0)
        if (off & 0x8000) {   // quad permute
            unsigned p[4] = {off & 3, (off >> 2) & 3, (off >> 4) & 3, (off >> 6) & 3};
            write_v(in.dst, fmt("quadperm(%s, %uu, %uu, %uu, %uu)", v.c_str(), p[0], p[1], p[2], p[3]));
            uses_quad = true;
        } else {              // bitmask mode within 32 lanes
            unsigned andm = off & 0x1f, orm = (off >> 5) & 0x1f, xorm = (off >> 10) & 0x1f;
            write_v(in.dst, fmt("xlane(%s, (L & ~31u) | ((((L & 31u) & %uu) | %uu) ^ %uu))", v.c_str(), andm, orm, xorm));
            uses_wave = true;
        }
        return true;
    }
    if (n == "ds_bpermute_b32") {
        // Wave64 permutes within each 32-lane half: source = (lane & 32) | ((addr >> 2) & 31); an
        // inactive source lane reads 0. In the two-lane mapping a half is one copy's PC wave.
        std::string srcl = dual || W == 32 ? fmt("((%s >> 2) & 31u)", a.c_str()) : fmt("((L & 32u) | ((%s >> 2) & 31u))", a.c_str());
        write_v(in.dst, fmt("bperm(%s, %s, kexec)", V(in.data0).c_str(), srcl.c_str()));
        uses_wave = true;
        return true;
    }
    if (n == "ds_permute_b32") return fail("ds_permute_b32 not supported");
    if (n == "ds_read_b32" || n == "ds_load_b32") { write_v(in.dst, fmt("lds[%s >> 2]", a.c_str())); return true; }
    if (n == "ds_read_b64" || n == "ds_read_b96" || n == "ds_read_b128") {
        unsigned cnt = n == "ds_read_b64" ? 2 : n == "ds_read_b96" ? 3 : 4;
        line(fmt("if (kexec) { uint a_ = %s >> 2;", a.c_str()));
        for (unsigned k = 0; k < cnt; k++) line(fmt("  %s = lds[a_ + %uu];", V(in.dst + k).c_str(), k));
        line("}");
        return true;
    }
    if (n == "ds_read2_b32" || n == "ds_read2st64_b32" || n == "ds_read2_b64" || n == "ds_read2st64_b64") {
        bool b64 = has(n, "b64");
        unsigned mul = (has(n, "st64") ? 64 : 1) * (b64 ? 8 : 4);
        line(fmt("if (kexec) { uint a0_ = (%s + %uu) >> 2, a1_ = (%s + %uu) >> 2;", V(in.vaddr).c_str(), uint32_t(in.offset_imm) * mul, V(in.vaddr).c_str(), uint32_t(in.gds_offset1) * mul));
        if (b64) line(fmt("  %s = lds[a0_]; %s = lds[a0_ + 1u]; %s = lds[a1_]; %s = lds[a1_ + 1u];", V(in.dst).c_str(), V(in.dst + 1).c_str(), V(in.dst + 2).c_str(), V(in.dst + 3).c_str()));
        else line(fmt("  %s = lds[a0_]; %s = lds[a1_];", V(in.dst).c_str(), V(in.dst + 1).c_str()));
        line("}");
        return true;
    }
    if (n == "ds_read_u8" || n == "ds_read_i8" || n == "ds_read_u16" || n == "ds_read_i16" || n == "ds_read_u16_d16" || n == "ds_read_u8_d16" ||
        n == "ds_read_u16_d16_hi" || n == "ds_read_u8_d16_hi") {
        bool b8 = has(n, "8");
        std::string v = fmt("((lds[%s >> 2] >> ((%s & 3u) * 8u)) & %s)", a.c_str(), a.c_str(), b8 ? "0xffu" : "0xffffu");
        if (n == "ds_read_i8") v = "uint(int(" + v + " << 24) >> 24)";
        if (n == "ds_read_i16") v = "uint(int(" + v + " << 16) >> 16)";
        if (has(n, "d16_hi")) write_v(in.dst, fmt("(%s & 0xffffu) | (%s << 16)", V(in.dst).c_str(), v.c_str()));
        else if (has(n, "d16")) write_v(in.dst, fmt("(%s & 0xffff0000u) | %s", V(in.dst).c_str(), v.c_str()));
        else write_v(in.dst, v);
        return true;
    }
    if (n == "ds_write_b32" || n == "ds_store_b32") { line(fmt("if (kexec) lds[%s >> 2] = %s;", a.c_str(), V(in.data0).c_str())); return true; }
    if (n == "ds_write_b64" || n == "ds_write_b96" || n == "ds_write_b128") {
        unsigned cnt = n == "ds_write_b64" ? 2 : n == "ds_write_b96" ? 3 : 4;
        line(fmt("if (kexec) { uint a_ = %s >> 2;", a.c_str()));
        for (unsigned k = 0; k < cnt; k++) line(fmt("  lds[a_ + %uu] = %s;", k, V(in.data0 + k).c_str()));
        line("}");
        return true;
    }
    if (n == "ds_write2_b32" || n == "ds_write2st64_b32" || n == "ds_write2_b64" || n == "ds_write2st64_b64") {
        bool b64 = has(n, "b64");
        unsigned mul = (has(n, "st64") ? 64 : 1) * (b64 ? 8 : 4);
        line(fmt("if (kexec) { uint a0_ = (%s + %uu) >> 2, a1_ = (%s + %uu) >> 2;", V(in.vaddr).c_str(), uint32_t(in.offset_imm) * mul, V(in.vaddr).c_str(), uint32_t(in.gds_offset1) * mul));
        if (b64) line(fmt("  lds[a0_] = %s; lds[a0_ + 1u] = %s; lds[a1_] = %s; lds[a1_ + 1u] = %s;", V(in.data0).c_str(), V(in.data0 + 1).c_str(), V(in.data1).c_str(), V(in.data1 + 1).c_str()));
        else line(fmt("  lds[a0_] = %s; lds[a1_] = %s;", V(in.data0).c_str(), V(in.data1).c_str()));
        line("}");
        return true;
    }
    if (n == "ds_write_b8" || n == "ds_write_b16" || n == "ds_write_b8_d16_hi" || n == "ds_write_b16_d16_hi") {
        bool b8 = has(n, "b8");
        std::string val = has(n, "d16_hi") ? "(" + V(in.data0) + " >> 16)" : V(in.data0);
        std::string m = b8 ? "0xffu" : "0xffffu";
        line(fmt("if (kexec) { uint a_ = %s; uint sh_ = (a_ & 3u) * 8u; InterlockedAnd(lds[a_ >> 2], ~(%s << sh_)); InterlockedOr(lds[a_ >> 2], (%s & %s) << sh_); }",
                 a.c_str(), m.c_str(), val.c_str(), m.c_str()));
        return true;
    }
    // Atomics: ds_<op>_u32 / ds_<op>_rtn_u32.
    static const char* m[][2] = {{"add", "InterlockedAdd"}, {"min_u32", "InterlockedMin"}, {"max_u32", "InterlockedMax"}, {"min_i32", "InterlockedMin"},
                                 {"max_i32", "InterlockedMax"}, {"and", "InterlockedAnd"}, {"or", "InterlockedOr"}, {"xor", "InterlockedXor"},
                                 {"wrxchg", "InterlockedExchange"}, {"sub", "InterlockedAdd"}, {"cmpst", "InterlockedCompareExchange"}};
    std::string op = n.substr(3);
    bool rtn = has(op, "_rtn_");
    std::string base = rtn ? op.substr(0, op.find("_rtn_")) + op.substr(op.find("_rtn_") + 4) : op;   // e.g. "add_u32"
    for (auto& p : m) {
        std::string k = p[0];
        bool match = base == k + "_u32" || base == k + "_b32" || base == k;
        if (!match) continue;
        bool sgn = has(k, "_i32");
        std::string val = V(in.data0);
        if (k == "sub") val = "(0u - " + val + ")";
        if (sgn) {
            // Group memory is declared unsigned: a signed minimum or maximum is a compare-exchange
            // loop on it (the value only moves one way, so a thread retries at most once per other
            // thread that got in first).
            const bool mx = k == "max_i32";
            line(fmt("if (kexec) { uint a_ = %s >> 2; uint o_ = lds[a_];", a.c_str()));
            line("  [loop] for (uint k_ = 0u; k_ < 4096u; k_++) {");
            line(fmt("    uint n_ = (int(%s) %s int(o_)) ? %s : o_;", val.c_str(), mx ? ">" : "<", val.c_str()));
            line("    if (n_ == o_) break;");
            line("    uint p_; InterlockedCompareExchange(lds[a_], o_, n_, p_);");
            line("    if (p_ == o_) break;");
            line("    o_ = p_;");
            line("  }");
            if (rtn) line(fmt("  %s = o_;", V(in.dst).c_str()));
            line("}");
            return true;
        }
        line("if (kexec) { uint o_;");
        if (k == "cmpst") line(fmt("  InterlockedCompareExchange(lds[%s >> 2], %s, %s, o_);", a.c_str(), V(in.data0).c_str(), V(in.data1).c_str()));
        else line(fmt("  %s(lds[%s >> 2], %s, o_);", p[1], a.c_str(), val.c_str()));
        if (rtn) line(fmt("  %s = uint(o_);", V(in.dst).c_str()));
        line("}");
        return true;
    }
    return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
}


// ---- pixel inputs (interpolation) ----------------------------------------------------------
// Console attribute N is the shader's Nth interpolated input (header records); its value is
// declared as an HLSL input with the original semantic, so the PC rasteriser interpolates it.
std::string Translator::attr_value(unsigned attr, unsigned chan, const std::string& mode) {
    auto it = attr_reg.find(attr);
    if (it == attr_reg.end()) { fail(fmt("pixel shader reads attribute %u that has no input record", attr)); return "0u"; }
    return attr_at(it->second, chan, mode);
}
// Component `chan` (a column of the register) of the pixel input in signature register `reg`, as
// bits: floats as their bit pattern, integers as they are.
std::string Translator::attr_at(unsigned reg, unsigned chan, const std::string& mode, const SigElem** elem) {
    const SigElem* el = nullptr;
    for (auto& e : isg)
        if (e.reg == reg && ((e.mask >> chan) & 1) && e.sysval == 0) el = &e;
    if (!el) {
        // A component past what the signature gives the register (the stage before writes two
        // components and the code interpolates a third it then does not use): nothing reaches it.
        for (auto& e : isg)
            if (e.reg == reg && e.sysval == 0) return "0u";
        fail(fmt("pixel input register %u component %u not in the input signature", reg, chan));
        return "0u";
    }
    unsigned first = unsigned(__builtin_ctz(el->mask));
    std::string md = el->is_float() ? mode : "nointerpolation";
    auto m = attr_mode.find(el->reg * 4 + first);
    if (m == attr_mode.end()) attr_mode[el->reg * 4 + first] = md;
    else if (m->second != md) { fail(fmt("pixel input %s interpolated in two modes", el->name.c_str())); return "0u"; }
    unsigned comp = chan - first;
    unsigned w = unsigned(__builtin_popcount(el->mask));
    std::string f = w > 1 ? fmt("I.a%u_%u.%c", el->reg, first, "xyzw"[comp]) : fmt("I.a%u_%u", el->reg, first);
    if (elem) *elem = el;
    return el->is_float() ? "asuint(" + f + ")" : "uint(" + f + ")";
}

// Half-float interpolation (v_interp_p1ll_f16 / p1lv / p2_f16). The first source field is not a
// register: it names the attribute (bits 0-5), a dword of the parameter (bits 6-7) and its upper
// half (bit 8); the second is the barycentric. The input's record says how 16-bit values lie in
// the parameter (Meta::PsInput). As with the 32-bit pair both steps write the finished value,
// here as a 16-bit pattern: the PC input is a 32-bit float the rasterizer has interpolated.
bool Translator::emit_interp16(const Instr& in) {
    const unsigned raw = in.src0 & 0x1ffu, attr = raw & 63u, dword = (raw >> 6) & 3u, high = raw >> 8;
    const Meta::PsInput* rec = nullptr;
    for (auto& r : meta.ps_inputs)
        if (r.attr == attr) rec = &r;
    if (!rec && attr < meta.ps_inputs.size()) rec = &meta.ps_inputs[attr];
    if (!rec) return fail(fmt("%s at 0x%x: attribute %u has no input record", in.name, in.offset, attr));
    if (!rec->fp16) return fail(fmt("%s at 0x%x: attribute %u is not a 16-bit input", in.name, in.offset, attr));
    unsigned row = rec->reg, index = dword;
    if (rec->pack2) {
        // The register's own elements take half a dword each (the record's mask byte does not say
        // how many dwords: it is 3 for every packed record).
        unsigned n1 = 0;
        for (auto& e : isg)
            if (e.reg == rec->reg && e.sysval == 0) n1 += unsigned(__builtin_popcount(e.mask));
        const unsigned first = (n1 + 1) / 2;
        if (dword < first) index = 2 * dword + high;
        else {
            const int partner = half_partner(rec->reg, true);
            if (partner < 0) return fail(fmt("%s at 0x%x: attribute %u dword %u has no second register", in.name, in.offset, attr, dword));
            row = unsigned(partner);
            index = 2 * (dword - first) + high;
        }
    } else if (high) return fail(fmt("%s at 0x%x: upper half of an unpacked 16-bit attribute", in.name, in.offset));
    // The register's elements start at its lowest column.
    unsigned col0 = 4;
    for (auto& e : isg)
        if (e.reg == row && e.sysval == 0 && e.mask) col0 = std::min(col0, unsigned(__builtin_ctz(e.mask)));
    if (col0 == 4) return fail(fmt("%s at 0x%x: pixel input register %u not in the input signature", in.name, in.offset, row));
    auto b = bary_mode.find(unsigned(in.src1 - src::kVgpr0));
    std::string w = b == bary_mode.end() ? "center" : b->second;
    std::string mode = w == "center" ? "linear" : w == "centroid" ? "centroid" : w == "sample" ? "sample" : w == "ncenter" ? "noperspective"
         : w == "ncentroid" ? "noperspective centroid" : w == "nsample" ? "noperspective sample" : "linear";
    if (rec->flat) mode = "nointerpolation";
    const SigElem* el = nullptr;
    std::string v = attr_at(row, col0 + index, mode, &el);
    if (!err.empty() || !el) return false;
    write_v(in.dst, el->is_float() ? "f32tof16(asfloat(" + v + "))" : "(" + v + " & 0xffffu)");
    return true;
}

bool Translator::emit_vintrp(const Instr& in, size_t idx) {
    std::string n = in.name;
    unsigned vsrc = in.src0 - src::kVgpr0;
    std::string mode;
    if (n == "v_interp_mov_f32") mode = "nointerpolation";
    else {
        auto b = bary_mode.find(vsrc);
        std::string w = b == bary_mode.end() ? "center" : b->second;
        mode = w == "center" ? "linear" : w == "centroid" ? "centroid" : w == "sample" ? "sample" : w == "ncenter" ? "noperspective"
             : w == "ncentroid" ? "noperspective centroid" : w == "nsample" ? "noperspective sample" : "linear";
    }
    // p1 produces an intermediate that only p2 consumes; both write the interpolated value.
    write_v(in.dst, attr_value(in.attr, in.attr_chan, mode));
    return true;
}

// ---- parameter exports by the stage's output records -----------------------------------------
// The second register of a packed pair of 16-bit values: the next 16-bit register of the
// signature that no record names, taken in register order by the packed records (-1: none). The
// same on both sides: vertex and mesh outputs, and pixel inputs (whose records hold a byte that
// names it in most shaders, but names the register itself in some that do have a second one).
int Translator::half_partner(unsigned row, bool pixel) {
    std::vector<std::pair<unsigned, bool>> recs;   // register, packed: the 16-bit records
    std::vector<unsigned> named;
    if (pixel) {
        for (auto& i : meta.ps_inputs) { named.push_back(i.reg); if (i.fp16) recs.push_back({i.reg, i.pack2}); }
    } else {
        for (auto& o : meta.vs_outputs) {
            if (o.prim) continue;
            named.push_back(o.row);
            if (o.fp16) recs.push_back({o.row, o.pack2});
        }
    }
    std::vector<unsigned> free;
    for (auto& e : pixel ? isg : osg) {
        if (e.sysval != 0 || !e.is_half()) continue;
        if (std::find(named.begin(), named.end(), e.reg) == named.end() && std::find(free.begin(), free.end(), e.reg) == free.end()) free.push_back(e.reg);
    }
    std::sort(free.begin(), free.end());
    std::sort(recs.begin(), recs.end());
    for (auto& o : recs) {
        int got = -1;
        if (o.second)
            for (size_t k = 0; k < free.size(); k++)
                if (free[k] > o.first) { got = int(free[k]); free.erase(free.begin() + k); break; }
        if (o.first == row) return got;
    }
    return -1;
}
// `exp param<p>`: each dword goes to the signature register its record names. 16-bit outputs sit
// in the low halves, or two per dword (element 2d low, 2d + 1 high), the register's own elements
// first and its partner's from the next whole dword; the PC outputs are 32-bit floats.
bool Translator::emit_param_records(const Instr& in, unsigned p, const std::string comp[4]) {
    const Meta::VsOutput* rec = nullptr;
    for (auto& o : meta.vs_outputs)
        if (o.param == p) rec = &o;
    if (!rec || (rec->prim && c.kind != ProgramKind::MS)) return true;   // exported but not consumed
    const std::vector<SigElem>& sig = rec->prim ? psg : osg;
    const char* obj = rec->prim ? "P.p" : "O.o";
    // One element (index-th component of the register's elements, in column order) = value.
    auto put = [&](unsigned row, unsigned index, const std::string& bits16or32, bool half) {
        unsigned seen = 0;
        std::vector<const SigElem*> els;
        for (auto& e : sig)
            if (e.sysval == 0 && e.reg == row) els.push_back(&e);
        std::sort(els.begin(), els.end(), [](const SigElem* a, const SigElem* b) { return (a->mask & -a->mask) < (b->mask & -b->mask); });
        for (auto* e : els) {
            const unsigned first = unsigned(__builtin_ctz(e->mask));
            for (unsigned k = 0; k < 4; k++) {
                if (!((e->mask >> k) & 1)) continue;
                if (seen++ != index) continue;
                std::string v;
                if (half) v = e->is_float() ? "f16tof32(" + bits16or32 + ")" : e->is_sint() ? "(int(" + bits16or32 + " << 16u) >> 16)" : bits16or32;
                else v = e->is_float() ? "asfloat(" + bits16or32 + ")" : e->is_sint() ? "int(" + bits16or32 + ")" : bits16or32;
                line(fmt("  %s%u_%u.%c = %s;", obj, e->reg, first, "xyzw"[k - first], v.c_str()));
                return;
            }
        }
    };
    auto channels = [&](unsigned row) {
        unsigned n = 0;
        for (auto& e : sig)
            if (e.sysval == 0 && e.reg == row) n += unsigned(__builtin_popcount(e.mask));
        return n;
    };
    line("if (kexec) {");
    const int partner = rec->pack2 && !rec->prim ? half_partner(rec->row) : -1;
    const unsigned n1 = channels(rec->row), d1 = (n1 + 1) / 2;
    for (unsigned k = 0; k < 4; k++) {
        if (!((in.exp_en >> k) & 1)) continue;
        const std::string bits = "asuint(" + comp[k] + ")";
        if (!rec->fp16) {
            // (A 32-bit register: dword k is column k of the register.)
            for (auto& e : sig) {
                if (e.sysval != 0 || e.reg != rec->row || !((e.mask >> k) & 1)) continue;
                const unsigned first = unsigned(__builtin_ctz(e.mask));
                std::string v = e.is_float() ? comp[k] : e.is_sint() ? "int(" + bits + ")" : bits;
                line(fmt("  %s%u_%u.%c = %s;", obj, e.reg, first, "xyzw"[k - first], v.c_str()));
            }
        } else if (!rec->pack2) {
            put(rec->row, k, "(" + bits + " & 0xffffu)", true);
        } else {
            for (unsigned h = 0; h < 2; h++) {
                const std::string half = h ? "(" + bits + " >> 16u)" : "(" + bits + " & 0xffffu)";
                if (k < d1) { if (2 * k + h < n1) put(rec->row, 2 * k + h, half, true); }
                else if (partner >= 0) put(unsigned(partner), 2 * (k - d1) + h, half, true);
            }
        }
    }
    line("}");
    return true;
}

// ---- exports --------------------------------------------------------------------------------
// VGPRs an instruction writes (for the packed-export peephole); conservative.
static bool writes_vgpr(const Instr& in, unsigned v) {
    switch (in.fmt) {
    case Fmt::VOP1: case Fmt::VOP2: case Fmt::VOP3: case Fmt::VOP3P: case Fmt::VINTRP:
        if (in.fmt == Fmt::VOP3 && in.op < 256) return false;
        if (in.name && (!strcmp(in.name, "v_readfirstlane_b32") || !strcmp(in.name, "v_readlane_b32"))) return false;
        return in.dst == v || in.dst + 1 == v;   // 64-bit results may write two
    case Fmt::MIMG: case Fmt::MUBUF: case Fmt::MTBUF: case Fmt::GLOBAL: case Fmt::FLAT: case Fmt::SCRATCH:
        return v >= in.vdata && v < in.vdata + 5u;
    case Fmt::DS: return v >= in.dst && v < in.dst + 4u;
    default: return false;
    }
}

bool Translator::emit_exp(const Instr& in, size_t idx) {
    const unsigned t = in.exp_target;
    // Packed fp16 exports come from v_cvt_pkrtz_f16_f32: export its f32 sources instead (the
    // console driver patches the export for the render-target format; PC converts on write).
    auto unpacked = [&](unsigned reg, std::string& lo, std::string& hi) -> bool {
        size_t b = idx;
        while (b > 0) {
            b--;
            const Instr& w = ins[b];
            if (w.fmt == Fmt::SOPP && w.name && (starts(w.name, "s_branch") || starts(w.name, "s_cbranch") || !strcmp(w.name, "s_endpgm"))) break;
            if (!writes_vgpr(w, reg)) continue;
            if (!w.name || strcmp(w.name, "v_cvt_pkrtz_f16_f32") || w.sdwa || w.dpp || (w.fmt == Fmt::VOP3 && (w.abs || w.neg))) return false;
            for (size_t k = b + 1; k < idx; k++) {
                if (w.src0 >= src::kVgpr0 && writes_vgpr(ins[k], w.src0 - src::kVgpr0)) return false;
                if (w.src1 >= src::kVgpr0 && writes_vgpr(ins[k], w.src1 - src::kVgpr0)) return false;
            }
            lo = "asfloat(" + rd(w.src0, w, 'f') + ")";
            hi = "asfloat(" + rd(w.src1, w, 'f') + ")";
            return true;
        }
        return false;
    };
    std::string comp[4];
    if (in.exp_compr) {
        for (int k = 0; k < 2; k++) {
            std::string lo, hi;
            unsigned reg = in.exp_src[k];
            if (!unpacked(reg, lo, hi)) {
                lo = "f16tof32(" + V(reg) + ")";
                hi = "f16tof32(" + V(reg) + " >> 16)";
            }
            comp[2 * k] = lo;
            comp[2 * k + 1] = hi;
        }
    } else {
        for (int k = 0; k < 4; k++) comp[k] = "asfloat(" + V(in.exp_src[k]) + ")";
    }
    auto en = [&](int k) { return in.exp_compr ? ((in.exp_en >> (k / 2 * 2)) & 1) : ((in.exp_en >> k) & 1); };
    if (c.kind == ProgramKind::PS) {
        if (t <= 7) {
            ps_targets.insert(t);
            bool isint = false;
            for (auto& e : osg)
                if (e.sysval == 64 && e.index == t && !e.is_float()) isint = true;
            line("if (kexec) {");
            for (int k = 0; k < 4; k++)
                if (en(k)) line(fmt("  O.t%u.%c = %s;", t, "xyzw"[k], isint ? ("asuint(" + comp[k] + ")").c_str() : comp[k].c_str()));
            line("}");
        } else if (t == 8) {
            line("if (kexec) {");
            bool sig_depth = osg.empty();
            for (auto& e : osg) sig_depth |= starts(e.name, "SV_Depth");
            if ((in.exp_en & 1) && sig_depth) { ps_depth = true; line("  O.depth_ = " + comp[0] + ";"); }
            // Stencil and sample-mask slots are written only when the signature has those outputs
            // (the export's enable mask can cover slots the shader does not output).
            bool sig_st = osg.empty(), sig_cov = osg.empty();
            for (auto& e : osg) { sig_st |= e.name == "SV_StencilRef"; sig_cov |= e.name == "SV_Coverage"; }
            // Shader-written stencil (SV_StencilRef) is optional on PC and missing on NVIDIA GPUs; it
            // is dropped unless TranslateOptions::stencil_export is set.
            if ((in.exp_en & 2) && sig_st && opt.stencil_export) { ps_stencil = true; line("  O.stencil_ = asuint(" + comp[1] + ");"); }
            if ((in.exp_en & 4) && sig_cov) { ps_mask = true; line("  O.mask_ = asuint(" + comp[2] + ");"); }
            line("}");
        } else if (t != 9) {
            return fail(fmt("pixel export target %u not supported", t));
        }
        if (in.exp_done) { ps_discard = true; line("if (!kexec) discard;"); }
        return true;
    }
    if (c.kind == ProgramKind::VS || c.kind == ProgramKind::MS) {
        const bool ms = c.kind == ProgramKind::MS;
        if (ms && t == 20) {   // primitive: three 10-bit vertex indices, bit 31 = null primitive
            line(fmt("if (kexec) { prim_ = %s; hasp_ = true; }", V(in.exp_src[0]).c_str()));
            return true;
        }
        if (ms && (t == 12 || t >= 32) && in.exp_en) line("if (kexec) hasv_ = true;");
        if (t == 12 && !in.exp_en) return true;
        if (t == 12) {
            vs_pos = true;
            line("if (kexec) {");
            for (int k = 0; k < 4; k++)
                if (en(k)) line(fmt("  O.pos_.%c = %s;", "xyzw"[k], comp[k].c_str()));
            line("}");
            return true;
        }
        if (t >= 13 && t <= 15) {
            if (!in.exp_en) return true;
            // pos1 is the misc vector when the outputs have a render-target array or viewport index
            // (z = array index, w = viewport index; AMD's layout), then clip and cull distances.
            bool misc = false;
            for (auto& e : osg) misc |= e.sysval == 4 || e.sysval == 5;
            unsigned k = t - 13;
            if (misc && k == 0) {
                line("if (kexec) {");
                for (auto& e : osg) {
                    // Misc vector z: render-target array index in bits 10:0, viewport index in 19:16.
                    if (e.sysval == 4 && (in.exp_en & 4)) line("  O.rtai_ = asuint(" + comp[2] + ") & 0x7ffu;");
                    if (e.sysval == 5 && (in.exp_en & 4)) line("  O.vpi_ = (asuint(" + comp[2] + ") >> 16) & 0xfu;");
                }
                line("}");
                return true;
            }
            unsigned cv = k - (misc ? 1 : 0);   // clip/cull vector index (4 distances each)
            line("if (kexec) {");
            for (int j = 0; j < 4; j++)
                if ((in.exp_en >> j) & 1) line(fmt("  clipcull_[%u] = %s;", cv * 4 + j, comp[j].c_str()));
            line("}");
            uses_clipcull = true;
            return true;
        }
        if (t >= 32 && t < 64) {
            unsigned p = t - 32;
            vs_params.insert(p);
            if (!meta.vs_outputs.empty()) return emit_param_records(in, p, comp);
            // Param N is the Nth output register (non-system elements, register order).
            std::vector<unsigned> regs;
            for (auto& e : osg)
                if (e.sysval == 0 && std::find(regs.begin(), regs.end(), e.reg) == regs.end()) regs.push_back(e.reg);
            std::sort(regs.begin(), regs.end());
            if (p >= regs.size()) {
                // Mesh shaders: parameters after the vertex outputs are the per-primitive outputs of
                // the exporting thread's primitive (primitive signature, register order).
                std::vector<unsigned> pregs;
                for (auto& e : psg)
                    if (e.sysval == 0 && std::find(pregs.begin(), pregs.end(), e.reg) == pregs.end()) pregs.push_back(e.reg);
                std::sort(pregs.begin(), pregs.end());
                unsigned q = p - unsigned(regs.size());
                if (c.kind != ProgramKind::MS || q >= pregs.size()) return true;   // exported but not consumed
                line("if (kexec) {");
                for (auto& e : psg) {
                    if (e.sysval != 0 || e.reg != pregs[q]) continue;
                    unsigned first = unsigned(__builtin_ctz(e.mask));
                    for (int k = 0; k < 4; k++) {
                        if (!((e.mask >> k) & 1) || !en(k)) continue;
                        std::string v = e.is_float() ? comp[k] : "asuint(" + comp[k] + ")";
                        line(fmt("  P.p%u_%u.%c = %s;", e.reg, first, "xyzw"[k - first], v.c_str()));
                    }
                }
                line("}");
                return true;
            }
            unsigned reg = regs[p];
            line("if (kexec) {");
            for (auto& e : osg) {
                if (e.sysval != 0 || e.reg != reg) continue;
                unsigned first = unsigned(__builtin_ctz(e.mask));
                for (int k = 0; k < 4; k++) {
                    if (!((e.mask >> k) & 1) || !en(k)) continue;
                    std::string v = e.is_float() ? comp[k] : "asuint(" + comp[k] + ")";
                    line(fmt("  O.o%u_%u.%c = %s;", e.reg, first, "xyzw"[k - first], v.c_str()));
                }
            }
            line("}");
            return true;
        }
        return fail(fmt("vertex export target %u not supported", t));
    }
    return fail("exports in this stage not supported");
}

// ---- global loads: constant tables addressed relative to the program counter ----------------
bool Translator::emit_global(const Instr& in, size_t idx) {
    std::string n = in.name;
    bool load = starts(n, "global_load_"), store = starts(n, "global_store_"), atomic = starts(n, "global_atomic_");
    if (!load && !store && !atomic) return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
    if (atomic) {
        // A 32-bit atomic at a GPU address: the data register is the operand (compare-and-swap:
        // the new value, then the value compared with), the result register gets the old value
        // when the instruction asks for it. Integer operations map to the PC ones; float minimum
        // and maximum have none and are a compare-and-swap loop.
        static const struct { const char* name; unsigned op; } kOps[] = {
            {"global_atomic_add", 1}, {"global_atomic_sub", 2}, {"global_atomic_umin", 3}, {"global_atomic_umax", 4}, {"global_atomic_smin", 5},
            {"global_atomic_smax", 6}, {"global_atomic_and", 7}, {"global_atomic_or", 8}, {"global_atomic_xor", 9}, {"global_atomic_swap", 10}};
        unsigned op = 0;
        for (auto& o : kOps) if (n == o.name) op = o.op;
        const bool fmn = n == "global_atomic_fmin", fmx = n == "global_atomic_fmax", cas = n == "global_atomic_cmpswap";
        if (!op && !fmn && !fmx && !cas) return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
        if (!opt.global_memory && !opt.global_memory_test) return fail(fmt("%s at 0x%x: global atomics need the layer's global memory", in.name, in.offset));
        uses_gm = true;
        uses_gm_atomic = true;
        std::string alo = in.saddr != src::kNull ? fmt("(%s + %s)", S(in.saddr).c_str(), V(in.vaddr).c_str()) : V(in.vaddr);
        std::string ahi = in.saddr != src::kNull ? fmt("(%s + (%s + %s < %s ? 1u : 0u))", S(in.saddr + 1).c_str(), S(in.saddr).c_str(), V(in.vaddr).c_str(), S(in.saddr).c_str())
                                                 : V(in.vaddr + 1);
        line(fmt("if (kexec) { uint lo_ = %s, hi_ = %s; uint im_ = %du; uint al_ = lo_ + im_;", alo.c_str(), ahi.c_str(), in.offset_imm));
        line(fmt("  hi_ += %s; lo_ = al_;", in.offset_imm < 0 ? "(al_ > lo_ ? 0xffffffffu : 0u)" : "(al_ < lo_ ? 1u : 0u)"));
        if (op) line(fmt("  uint o_ = gm_atomic(lo_, hi_, %uu, %s);", op, V(in.vdata).c_str()));
        else if (cas) line(fmt("  uint o_ = gm_cmpswap(lo_, hi_, %s, %s);", V(in.vdata + 1).c_str(), V(in.vdata).c_str()));
        else line(fmt("  uint o_ = gm_atomic_f(lo_, hi_, %s, %s);", fmx ? "true" : "false", V(in.vdata).c_str()));
        if (in.glc) line(fmt("  %s = o_;", V(in.dst).c_str()));
        line("}");
        return true;
    }
    bool dword = has(n, "dword");
    unsigned cnt = has(n, "x4") ? 4 : has(n, "x3") ? 3 : has(n, "x2") ? 2 : 1;
    if (!dword && (store ? !(ends(n, "byte") || ends(n, "short")) : !(ends(n, "ubyte") || ends(n, "sbyte") || ends(n, "ushort") || ends(n, "sshort"))))
        return fail(fmt("%s at 0x%x not supported", in.name, in.offset));
    if (store && !opt.global_memory && !opt.global_memory_test) return fail(fmt("%s at 0x%x: global stores need the layer's global memory", in.name, in.offset));
    uses_codedata = true;
    if (opt.global_memory || opt.global_memory_test) uses_gm = true;
    // 64-bit address: SGPR base pair + 32-bit VGPR offset, or a VGPR pair; plus the signed immediate.
    std::string lo0 = in.saddr != src::kNull ? fmt("(%s + %s)", S(in.saddr).c_str(), V(in.vaddr).c_str()) : V(in.vaddr);
    std::string hi0 = in.saddr != src::kNull ? fmt("(%s + (%s + %s < %s ? 1u : 0u))", S(in.saddr + 1).c_str(), S(in.saddr).c_str(), V(in.vaddr).c_str(), S(in.saddr).c_str())
                                             : V(in.vaddr + 1);
    line(fmt("if (kexec) { uint lo_ = %s, hi_ = %s; uint im_ = %du; uint al_ = lo_ + im_;", lo0.c_str(), hi0.c_str(), in.offset_imm));
    line(fmt("  hi_ += %s; lo_ = al_;", in.offset_imm < 0 ? "(al_ > lo_ ? 0xffffffffu : 0u)" : "(al_ < lo_ ? 1u : 0u)"));
    auto rdw = [&](const std::string& k) {   // dword at lo_ + k
        std::string cd = fmt("((lo_ + %s) >> 2) < CODEDATA_N ? codedata[(lo_ + %s) >> 2] : 0u", k.c_str(), k.c_str());
        if (!opt.global_memory && !opt.global_memory_test) return fmt("(hi_ == 0u ? (%s) : 0u)", cd.c_str());
        return fmt("(hi_ == 0u && ((lo_ + %s) >> 2) < CODEDATA_N ? codedata[(lo_ + %s) >> 2] : gm_load(lo_ + %s, hi_ + (lo_ + %s < lo_ ? 1u : 0u)))",
                   k.c_str(), k.c_str(), k.c_str(), k.c_str());
    };
    if (load && dword && cnt > 1 && (opt.global_memory || opt.global_memory_test)) {
        line("  uint fy_, fz_, fl_; bool fx_ = gm_find(lo_, hi_, fy_, fz_, fl_);");
        for (unsigned k = 0; k < cnt; k++)
            line(fmt("  %s = hi_ == 0u && ((lo_ + %uu) >> 2) < CODEDATA_N ? codedata[(lo_ + %uu) >> 2] : (fx_ && fl_ > %uu ? gm_vload(fy_, fz_ + %uu) : 0u);",
                     V(in.dst + k).c_str(), 4 * k, 4 * k, 4 * k, 4 * k));
    } else if (load && dword) {
        for (unsigned k = 0; k < cnt; k++) line(fmt("  %s = %s;", V(in.dst + k).c_str(), rdw(fmt("%uu", 4 * k)).c_str()));
    } else if (load) {
        bool sgn = has(n, "sbyte") || has(n, "sshort");
        unsigned bits = has(n, "byte") ? 8 : 16;
        line(fmt("  uint w_ = %s; uint v_ = (w_ >> ((lo_ & 3u) * 8u)) & 0x%xu;", rdw("0u").c_str(), (1u << bits) - 1));
        line(fmt("  %s = %s;", V(in.dst).c_str(), sgn ? fmt("uint(int(v_ << %uu) >> %u)", 32 - bits, 32 - bits).c_str() : "v_"));
    } else if (dword) {
        for (unsigned k = 0; k < cnt; k++) line(fmt("  gm_store(lo_ + %uu, hi_ + (lo_ + %uu < lo_ ? 1u : 0u), %s);", 4 * k, 4 * k, V(in.vdata + k).c_str()));
    } else {
        unsigned bits = ends(n, "byte") ? 8 : 16;
        line(fmt("  gm_store_bits(lo_, hi_, %s & 0x%xu, 0x%xu);", V(in.vdata).c_str(), (1u << bits) - 1, (1u << bits) - 1));
    }
    line("}");
    return true;
}

// ---- declarations and program ------------------------------------------------------------
std::string Translator::res_decl(const Res& r) const {
    std::string reg;
    std::string arr = r.dyn ? (r.array ? fmt("[%u]", r.array) : std::string("[]")) : "";
    char rc = r.kind == ResKind::CBuffer ? 'b' : (r.kind == ResKind::Sampler || r.kind == ResKind::StaticSampler) ? 's' : r.uav ? 'u' : 't';
    reg = fmt(": register(%c%u, space%u)", rc, r.reg, r.space);
    const char* rw = r.uav ? "RW" : "";
    switch (r.kind) {
    case ResKind::CBuffer: return fmt("cbuffer CB_%s %s { uint4 %s_d[4096]; };", r.name.c_str(), reg.c_str(), r.name.c_str());
    case ResKind::RawBuf: return fmt("%sByteAddressBuffer %s%s %s;", rw, r.name.c_str(), arr.c_str(), reg.c_str());
    case ResKind::StructBuf: return fmt("struct E_%s { uint w[%u]; };\n%sStructuredBuffer<E_%s> %s%s %s;", r.name.c_str(), std::max(1u, r.stride / 4), rw, r.name.c_str(), r.name.c_str(), arr.c_str(), reg.c_str());
    case ResKind::TypedBuf: return fmt("%sBuffer<%s%s> %s%s %s;", rw, r.is_uint ? "uint" : "float", r.scalar ? "" : "4", r.name.c_str(), arr.c_str(), reg.c_str());
    case ResKind::Image: {
        int dim = r.dim < 0 ? 1 : r.dim;
        std::string t = std::string(r.uav ? "RWTexture" : "Texture") + (dim == 3 ? std::string("Cube") : dim == 4 ? "1DArray" : dim == 5 ? "2DArray" : dim == 6 ? "2DMS" : dim == 7 ? "2DMSArray" : kDimName[dim]);
        if (dim == 3 && r.cube_array) t = r.uav ? "RWTexture2DArray" : "TextureCubeArray";
        if (r.uav && dim == 3) t = "RWTexture2DArray";
        return fmt("%s<%s%s> %s%s %s;", t.c_str(), r.is_uint ? "uint" : "float", r.scalar ? "" : "4", r.name.c_str(), arr.c_str(), reg.c_str());
    }
    case ResKind::Sampler: case ResKind::StaticSampler:
        return fmt("%s %s%s %s;", r.cmp_sampler ? "SamplerComparisonState" : "SamplerState", r.name.c_str(), arr.c_str(), reg.c_str());
    }
    return "";
}

// image_bvh*_intersect_ray over global memory. Node id: address >> 3 | type (0-3 triangle, 4 box16,
// 5 box32). Box nodes: four child ids, then per child min xyz, max xyz (box32: floats, box16: f16
// pairs); a hit child's id is returned, misses 0xFFFFFFFF, sorted by entry distance when asked.
// Triangle nodes: v0, v1, v2 (floats); returns (t*det, det, u*det, v*det) so that t = x / y, with t =
// +inf on a miss. Other node types are left to the shader (all 0xFFFFFFFF). Layouts follow AMD's
// hardware node formats as Mesa (RADV, MIT) documents them.
const char* kBvhPrelude = R"(
float gmf(uint64_t a) { return asfloat(gm_load(uint(a), uint(a >> 32))); }
uint gmu(uint64_t a) { return gm_load(uint(a), uint(a >> 32)); }
uint4 bvh_isect(uint64_t base, uint64_t ptr, float tmax, float3 o, float3 d, float3 inv, bool sortc) {
  uint type = uint(ptr) & 7u;
  uint64_t a = base + ((ptr & ~7ull) << 3);
  if (type == 4u || type == 5u) {
    uint ch[4]; float tn[4];
    [unroll] for (uint i = 0u; i < 4u; i++) {
      ch[i] = gmu(a + 4u * i);
      float3 mn, mx;
      if (type == 5u) {
        uint64_t c = a + 16u + 24u * i;
        mn = float3(gmf(c), gmf(c + 4u), gmf(c + 8u));
        mx = float3(gmf(c + 12u), gmf(c + 16u), gmf(c + 20u));
      } else {
        uint64_t c = a + 16u + 12u * i;
        uint p0 = gmu(c), p1 = gmu(c + 4u), p2 = gmu(c + 8u);
        mn = float3(f16tof32(p0), f16tof32(p0 >> 16), f16tof32(p1));
        mx = float3(f16tof32(p1 >> 16), f16tof32(p2), f16tof32(p2 >> 16));
      }
      float3 t1 = (mn - o) * inv, t2 = (mx - o) * inv;
      float lo = max(max(min(t1.x, t2.x), min(t1.y, t2.y)), min(t1.z, t2.z));
      float hi = min(min(max(t1.x, t2.x), max(t1.y, t2.y)), max(t1.z, t2.z));
      bool hit = ch[i] != 0xffffffffu && lo <= hi && hi >= 0.0f && lo <= tmax;
      tn[i] = hit ? lo : 3.0e38f;
      if (!hit) ch[i] = 0xffffffffu;
    }
    if (sortc) {
      [unroll] for (uint p = 0u; p < 3u; p++)
        [unroll] for (uint q = 0u; q < 3u - p; q++)
          if (tn[q + 1u] < tn[q]) { float tt = tn[q]; tn[q] = tn[q + 1u]; tn[q + 1u] = tt; uint cc = ch[q]; ch[q] = ch[q + 1u]; ch[q + 1u] = cc; }
    }
    return uint4(ch[0], ch[1], ch[2], ch[3]);
  }
  if (type <= 3u) {
    float3 v0 = float3(gmf(a), gmf(a + 4u), gmf(a + 8u));
    float3 v1 = float3(gmf(a + 12u), gmf(a + 16u), gmf(a + 20u));
    float3 v2 = float3(gmf(a + 24u), gmf(a + 28u), gmf(a + 32u));
    float3 e1 = v1 - v0, e2 = v2 - v0, p = cross(d, e2);
    float det = dot(e1, p);
    float3 tv = o - v0, q = cross(tv, e1);
    float u = dot(tv, p), v = dot(d, q), t = dot(e2, q);
    float s = det < 0.0f ? -1.0f : 1.0f;
    bool ok = det != 0.0f && u * s >= 0.0f && v * s >= 0.0f && (u + v) * s <= det * s;
    return ok ? uint4(asuint(t), asuint(det), asuint(u), asuint(v)) : uint4(0x7f800000u, 0x3f800000u, 0u, 0u);
  }
  return uint4(0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
}
)";

const char* kPrelude = R"(
static uint L, LG;
static uint64_t memtime_ = 0;
// 32x32 -> high 32 bits without 64-bit integer multiplies (some PC drivers reject those).
static uint pops_n_ = 0u;
uint pops_next() { uint v = pops_n_ << 30; pops_n_++; return v; }   // (see kPopsExitingWaveId)
uint mulhi_u(uint a, uint b) {
  uint al = a & 0xffffu, ah = a >> 16, bl = b & 0xffffu, bh = b >> 16;
  uint ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
  uint mid = (ll >> 16) + (lh & 0xffffu) + (hl & 0xffffu);
  return hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}
int mulhi_i(int a, int b) { uint h = mulhi_u(uint(a), uint(b)); if (a < 0) h -= uint(b); if (b < 0) h -= uint(a); return int(h); }
bool lanebit(uint lo, uint hi) { return L < 32u ? ((lo >> L) & 1u) != 0u : ((hi >> (L - 32u)) & 1u) != 0u; }
bool lanebit1(uint lo) { return ((lo >> (L & 31u)) & 1u) != 0u; }
uint2 ballot2(bool b) { uint4 r = WaveActiveBallot(b); return r.xy; }
uint first_lane(bool e) { uint2 b = ballot2(e); return b.x != 0u ? firstbitlow(b.x) : (b.y != 0u ? 32u + firstbitlow(b.y) : 0u); }
uint ff1_64(uint lo, uint hi) { return lo != 0u ? firstbitlow(lo) : (hi != 0u ? 32u + firstbitlow(hi) : 0xffffffffu); }
uint flbit_i32(uint x) { int v = int(x); if (v == 0 || v == -1) return 0xffffffffu; return 31u - firstbithigh(v < 0 ? ~x : x); }
uint wqm32(uint x) { uint r = 0u; [unroll] for (uint q = 0u; q < 32u; q += 4u) if (((x >> q) & 15u) != 0u) r |= 15u << q; return r; }
float ndiv(float x, float d) { float q = x * (1.0f / d); float e = mad(-q, d, x); return mad(e, 1.0f / d, q); }   // x / d, correctly rounded for small integers
float mul_legacy(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float mad_legacy(float a, float b, float c) { precise float p = mul_legacy(a, b); precise float r = p + c; return r; }   // unfused
float fma_legacy(float a, float b, float c) { return (a == 0.0f || b == 0.0f) ? c : mad(a, b, c); }                       // fused
uint cvt_u32(float f) { return (f != f || f <= 0.0f) ? 0u : (f >= 4294967040.0f ? 0xffffffffu : (uint)f); }
int cvt_i32(float f) { return f != f ? 0 : (f <= -2147483648.0f ? int(0x80000000u) : (f >= 2147483520.0f ? 0x7fffffff : (int)f)); }
uint bfe_u(uint x, uint o, uint w) { if (w == 0u) return 0u; uint v = x >> o; return w >= 32u ? v : v & ((1u << w) - 1u); }
int bfe_i(int x, uint o, uint w) { if (w == 0u) return 0; if (o + w >= 32u) return x >> o; return (x << (32u - o - w)) >> (32u - w); }
uint64_t bfe_u64(uint64_t x, uint o, uint w) { if (w == 0u) return 0; uint64_t v = x >> o; return w >= 64u ? v : v & ((uint64_t(1) << w) - 1); }
float med3f(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
float frexp_mant(float a) { float e; return frexp(a, e); }
int frexp_exp(float a) { float e; frexp(a, e); return int(e); }
// f32 -> f16 with round-to-nearest-even (f16r) and round-toward-zero (f16z), whatever rounding the
// PC's own conversion uses: its result is corrected by one f16 step when the neighbour is closer.
uint f16r(float x) {
  uint h = f32tof16(x) & 0xffffu;
  if (isnan(x)) return h;
  float a = abs(x);
  uint s = asuint(x) >> 31 << 15;
  if (a >= 65520.0) return s | 0x7c00u;
  uint m = h & 0x7fffu;
  float v = f16tof32(m);
  if (v == a) return s | m;
  uint n = v < a ? m + 1u : m - 1u;
  float dv = abs(v - a), dn = abs(f16tof32(n) - a);
  return s | ((dn < dv || (dn == dv && (n & 1u) == 0u)) ? n : m);
}
uint f16z(float x) {
  uint h = f16r(x);
  if (isnan(x) || isinf(x)) return h;
  uint m = h & 0x7fffu;
  if (m != 0u && f16tof32(m) > abs(x)) m -= 1u;
  if (m == 0x7c00u) m = 0x7bffu;   // finite values do not round toward zero to infinity
  return (h & 0x8000u) | m;
}
uint pkrtz(float a, float b) { return f16z(a) | (f16z(b) << 16); }
uint pknorm_u16(float a, float b) { return uint(round(saturate(a) * 65535.0)) | (uint(round(saturate(b) * 65535.0)) << 16); }
uint pknorm_i16(float a, float b) { return (uint(int(round(clamp(a, -1.0, 1.0) * 32767.0))) & 0xffffu) | (uint(int(round(clamp(b, -1.0, 1.0) * 32767.0))) << 16); }
uint pk_u8(float a, uint sel, uint c) { uint s = (sel & 3u) * 8u; return (c & ~(0xffu << s)) | ((cvt_u32(round(a)) & 0xffu) << s); }
uint perm_b32(uint a, uint b, uint sel) {
  uint r = 0u;
  [unroll] for (uint i = 0u; i < 4u; i++) {
    uint s = (sel >> (i * 8u)) & 0xffu, v;
    if (s < 4u) v = (b >> (s * 8u)) & 0xffu; else if (s < 8u) v = (a >> ((s - 4u) * 8u)) & 0xffu;
    else if (s == 12u) v = 0u; else if (s > 12u) v = 0xffu;
    else { uint bit = s == 8u ? ((b >> 15) & 1u) : s == 9u ? ((b >> 31) & 1u) : s == 10u ? ((a >> 15) & 1u) : ((a >> 31) & 1u); v = bit * 0xffu; }
    r |= v << (i * 8u);
  }
  return r;
}
uint lerp_u8(uint a, uint b, uint c) { uint r = 0u; [unroll] for (uint i = 0u; i < 32u; i += 8u) r |= ((((a >> i) & 0xffu) + ((b >> i) & 0xffu) + ((c >> i) & 1u)) >> 1) << i; return r; }
uint sad_u8(uint a, uint b, uint c) { uint r = c; [unroll] for (uint i = 0u; i < 32u; i += 8u) { uint x = (a >> i) & 0xffu, y = (b >> i) & 0xffu; r += max(x, y) - min(x, y); } return r; }
uint msad_u8(uint a, uint b, uint c) { uint r = c; [unroll] for (uint i = 0u; i < 32u; i += 8u) { uint x = (a >> i) & 0xffu, y = (b >> i) & 0xffu; if (y != 0u) r += max(x, y) - min(x, y); } return r; }
float dot2_f16(uint a, uint b, float c) { return f16tof32(a) * f16tof32(b) + f16tof32(a >> 16) * f16tof32(b >> 16) + c; }
bool fclass(float x, uint m) {
  uint u = asuint(x); bool neg = (u >> 31) != 0u; uint e = (u >> 23) & 0xffu, f = u & 0x7fffffu;
  uint c = e == 0xffu ? (f != 0u ? ((f & 0x400000u) != 0u ? 2u : 1u) : (neg ? 4u : 512u))
         : e == 0u ? (f == 0u ? (neg ? 32u : 64u) : (neg ? 16u : 128u)) : (neg ? 8u : 256u);
  return (m & c) != 0u;
}
bool fclass64(double x, uint m) { return fclass(float(x), m); }
// Cube face coordinates (AMD v_cube* semantics) and the direction they encode.
float cube_ma(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return 2.0 * (az >= ax && az >= ay ? z : ay >= ax ? y : x); }
float cube_id(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return az >= ax && az >= ay ? (z < 0.0 ? 5.0 : 4.0) : ay >= ax ? (y < 0.0 ? 3.0 : 2.0) : (x < 0.0 ? 1.0 : 0.0); }
float cube_sc(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return az >= ax && az >= ay ? (z < 0.0 ? -x : x) : ay >= ax ? x : (x < 0.0 ? z : -z); }
float cube_tc(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return az >= ax && az >= ay ? -y : ay >= ax ? (y < 0.0 ? -z : z) : -y; }
float3 cube_dir(float s, float t, float f) {
  float u = (s - 1.5) * 2.0, v = (t - 1.5) * 2.0; uint face = uint(round(f)) % 8u;
  if (face == 0u) return float3(1.0, -v, -u); if (face == 1u) return float3(-1.0, -v, u);
  if (face == 2u) return float3(u, 1.0, v); if (face == 3u) return float3(u, -1.0, -v);
  if (face == 4u) return float3(u, -v, 1.0); return float3(-u, -v, -1.0);
}
float3 cube_grad(float gs, float gt, float f) {
  float u = gs * 2.0, v = gt * 2.0; uint face = uint(round(f)) % 8u;
  if (face == 0u) return float3(0.0, -v, -u); if (face == 1u) return float3(0.0, -v, u);
  if (face == 2u) return float3(u, 0.0, v); if (face == 3u) return float3(u, 0.0, -v);
  if (face == 4u) return float3(u, -v, 0.0); return float3(-u, -v, 0.0);
}
uint64_t asuint64(double d) { uint lo_, hi_; asuint(d, lo_, hi_); return uint64_t(lo_) | (uint64_t(hi_) << 32); }
// Cross-lane reads (exact within one PC wave).
#ifndef XL_N
#define XL_N WAVE
#endif
uint xlane(uint v, uint src) { uint r = 0u; [unroll] for (uint k = 0u; k < XL_N; k++) { uint t = WaveReadLaneAt(v, k); if ((src % XL_N) == k) r = t; } return r; }
bool lanebitL(uint lo, uint hi, uint l) { return l < 32u ? ((lo >> l) & 1u) != 0u : ((hi >> (l - 32u)) & 1u) != 0u; }
uint first_lane64(uint lo, uint hi) { return lo != 0u ? firstbitlow(lo) : (hi != 0u ? 32u + firstbitlow(hi) : 0u); }
// v_mad/v_mac (not v_fma): the product is rounded before the add.
float umad(float a, float b, float c) { precise float p = a * b; precise float r = p + c; return r; }
uint bperm(uint v, uint src, bool act) { uint r = xlane(v, src), on = xlane(act ? 1u : 0u, src); return on != 0u ? r : 0u; }
uint xlane64(uint v0, uint v1, uint src) { uint a = xlane(v0, src & 31u), b = xlane(v1, src & 31u); return src < 32u ? a : b; }
uint readlane64(uint v0, uint v1, uint l) { return l < 32u ? WaveReadLaneAt(v0, l) : WaveReadLaneAt(v1, l - 32u); }
uint xlane_bc(uint v, uint src, bool bc) { uint r = xlane(v, src & (WAVE - 1u)); return src == 0xffffffffu ? (bc ? 0u : v) : r; }
#define cbload(d, a) (d)[((a) >> 4) & 4095u][((a) >> 2) & 3u]
)";

const char* kQuadPrelude = R"(
uint quadperm(uint v, uint p0, uint p1, uint p2, uint p3) {
  uint q0 = QuadReadLaneAt(v, 0u), q1 = QuadReadLaneAt(v, 1u), q2 = QuadReadLaneAt(v, 2u), q3 = QuadReadLaneAt(v, 3u);
  uint l = L & 3u; uint p = l == 0u ? p0 : l == 1u ? p1 : l == 2u ? p2 : p3;
  return p == 0u ? q0 : p == 1u ? q1 : p == 2u ? q2 : q3;
}
)";

std::string Translator::generate() {
    // Body first (it decides resource usage), then wrap with declarations.
    const bool loops = indirect || [&] {
        for (auto& in : ins)
            if (in.fmt == Fmt::SOPP && (starts(in.name, "s_branch") || starts(in.name, "s_cbranch")) && in.simm16 < 0) return true;
        return false;
    }();
    std::ostringstream body;
    std::swap(out, body);
    std::string cond_of;
    auto branch_cond = [&](const Instr& last) -> std::string {
        std::string ln = last.name;
        if (ln == "s_cbranch_scc0") return "!scc";
        if (ln == "s_cbranch_scc1") return "scc";
        if (ln == "s_cbranch_vccz") return W == 64 ? "(s106 | s107) == 0u" : "s106 == 0u";
        if (ln == "s_cbranch_vccnz") return W == 64 ? "(s106 | s107) != 0u" : "s106 != 0u";
        if (ln == "s_cbranch_execz") return W == 64 ? "(s126 | s127) == 0u" : "s126 == 0u";
        if (ln == "s_cbranch_execnz") return W == 64 ? "(s126 | s127) != 0u" : "s126 != 0u";
        fail(ln + " not supported");
        return "false";
    };
    if (W == 32) {
        // Masks only use the low SGPR; keep the high views zero so the shared conditions work.
    }
    const size_t nb = block_start.size();
    // The blocks an invocation can still reach a barrier from (itself included). Past the last
    // barrier nobody waits for it any more, so there the small step limit applies again: a list
    // walk over data an earlier pass did not prepare otherwise runs to the large limit, at
    // hundreds of milliseconds of GPU time per dispatch.
    std::vector<bool> barrier_ahead(nb, true);
    bool barriers = false;
    bool has_barrier = false;   // (anywhere in the program)
    for (auto& i : ins) has_barrier |= i.name && !strcmp(i.name, "s_barrier");
    if (!loops) line(fmt("bool reach[%zu]; [unroll] for (uint i_ = 0u; i_ < %zuu; i_++) reach[i_] = false; reach[0] = true;", nb + 1, nb + 1));
    // The guard bounds the block transitions so a mistranslated loop ends the shader instead of
    // hanging the GPU (which removes the PC device).
    else {
        // A wave stopped by the guard leaves the group: only barriers it could still reach (at or
        // after a loop head) need the large limit.
        uint32_t first_head = UINT32_MAX;
        for (auto& i : ins)
            if (i.name && (starts(i.name, "s_cbranch") || !strcmp(i.name, "s_branch")) && i.simm16 < 0)
                first_head = std::min(first_head, uint32_t(int32_t(i.offset) + 4 + i.simm16 * 4));
        for (auto& i : ins) barriers |= i.name && !strcmp(i.name, "s_barrier") && i.offset >= first_head;
        if (barriers && !indirect) {
            auto succ_of = [&](size_t b, std::vector<size_t>& succ) {
                succ.clear();
                const Instr& last = ins[block_end(b) - 1];
                std::string ln = last.name ? last.name : "";
                if (last.fmt == Fmt::SOPP && (starts(ln, "s_branch") || starts(ln, "s_cbranch"))) {
                    auto it = block_of.find(uint32_t(int32_t(last.offset) + 4 + last.simm16 * 4));
                    if (it != block_of.end()) succ.push_back(it->second);
                }
                if (!(last.fmt == Fmt::SOPP && (ln == "s_endpgm" || starts(ln, "s_branch"))) && b + 1 < nb) succ.push_back(b + 1);
            };
            for (size_t b = 0; b < nb; b++) {
                barrier_ahead[b] = false;
                for (size_t i = block_start[b]; i < block_end(b); i++) barrier_ahead[b] = barrier_ahead[b] || (ins[i].name && !strcmp(ins[i].name, "s_barrier"));
            }
            std::vector<size_t> succ;
            for (bool changed = true; changed;) {
                changed = false;
                for (size_t b = nb; b-- > 0;) {
                    if (barrier_ahead[b]) continue;
                    succ_of(b, succ);
                    for (size_t x : succ)
                        if (barrier_ahead[x]) { barrier_ahead[b] = true; changed = true; break; }
                }
            }
        }
        size_t start = opt.raw_code.empty() || !block_of.count(opt.raw_entry) ? 0 : block_of[opt.raw_entry];
        coop = has_barrier && c.kind == ProgramKind::CS && opt.barrier_sync && !indirect && opt.raw_code.empty();
        if (coop)
            line(fmt("uint pc = %zuu, guard_ = 0u, guard2_ = 0u; bool atb_ = false;\n  [loop] for (uint round_ = 0u; round_ < 8192u; round_++) {\n  [loop] for (;;) {\n"
                     "  if (pc == 0xffffffffu || atb_) break;\n  if (++guard_ > %uu) { pc = 0xffffffffu; break; }\n  [forcecase] switch (pc) {", start, opt.loop_guard_barrier));
        else
        line(fmt("uint pc = %zuu, guard_ = 0u, guard2_ = 0u;\n  [loop] for (;;) {\n  if (++guard_ > %uu) %s\n  [forcecase] switch (pc) {", start,
                 barriers ? opt.loop_guard_barrier : opt.loop_guard, opt.raw_stats ? "{ dbg_add(5u); dbg_set2(19u, pc, guard_); RETURN; }" : "RETURN;"));
    }
    // Blocks not reachable from the entry (data after the last instruction) are not emitted.
    std::vector<bool> live(nb, indirect);
    if (!indirect) {
        std::vector<size_t> work{0};
        live[0] = true;
        while (!work.empty()) {
            size_t b = work.back();
            work.pop_back();
            const Instr& last = ins[block_end(b) - 1];
            std::string ln = last.name ? last.name : "";
            std::vector<size_t> succ;
            bool br = last.fmt == Fmt::SOPP && (starts(ln, "s_branch") || starts(ln, "s_cbranch"));
            if (br) {
                auto it = block_of.find(uint32_t(int32_t(last.offset) + 4 + last.simm16 * 4));
                if (it != block_of.end()) succ.push_back(it->second);
            }
            if (!(last.fmt == Fmt::SOPP && (ln == "s_endpgm" || starts(ln, "s_branch"))) && b + 1 < nb) succ.push_back(b + 1);
            for (size_t x : succ)
                if (!live[x]) { live[x] = true; work.push_back(x); }
        }
    }
    // Code with loops: a block that is only ever entered by falling into it (the code after a
    // conditional branch that nothing else jumps to) is not a case of its own: it goes on in the
    // case of the block before it. That halves the cases of the dispatch and takes most values
    // out of the merge at its head, which is what the compilers after this spend their time on
    // (a megabyte of translation took the HLSL compiler many minutes).
    std::vector<bool> entered(nb, !loops);
    if (loops) {
        entered[0] = true;
        if (!opt.raw_code.empty() && block_of.count(opt.raw_entry)) entered[block_of[opt.raw_entry]] = true;
        for (uint32_t t : jump_targets)
            if (block_of.count(t)) entered[block_of[t]] = true;
        for (size_t b = 0; b < nb; b++) {
            const Instr& last = ins[block_end(b) - 1];
            const std::string ln = last.name ? last.name : "";
            if (last.fmt == Fmt::SOPP && (starts(ln, "s_branch") || starts(ln, "s_cbranch"))) {
                auto it = block_of.find(uint32_t(int32_t(last.offset) + 4 + last.simm16 * 4));
                if (it != block_of.end()) entered[it->second] = true;
            }
            // (after a barrier the threads of the group meet: the case ends there; after the end
            // of the program or a computed jump nothing falls through)
            if (b + 1 < nb && (ln == "s_barrier" || ln == "s_endpgm" || ln == "s_setpc_b64" || ln == "s_swappc_b64")) entered[b + 1] = true;
        }
    }
    bool inCase = false;   // (the case of an earlier block is still open: this block continues it)
    for (size_t b = 0; b < nb; b++) {
        size_t e = block_end(b);
        if (!live[b]) { if (inCase) { line("RETURN;\n  }"); inCase = false; } continue; }
        lane_stale = true;   // (a block is entered from several places: what L holds there is not known)
        if (!loops) line(fmt("if (reach[%zu]) {", b));
        else if (!inCase) line(fmt("case %zuu: {", b));
        open_block = !loops && has_barrier ? int(b) : -1;
        if (loops && !inCase && barriers && !barrier_ahead[b])   // past the last barrier: the small limit (see barrier_ahead)
            line(fmt("if (++guard2_ > %uu) %s", opt.loop_guard, opt.raw_stats ? "{ dbg_add(5u); dbg_set2(19u, pc, guard_); RETURN; }" : "RETURN;"));
        // The next block continues this case when control only falls into it.
        const bool goesOn = loops && b + 1 < nb && live[b + 1] && !entered[b + 1];
        inCase = false;
        jumped = false;
        for (size_t i = block_start[b]; i < e; i++)
            if (!emit(ins[i], i) && !err.empty()) break;
        if (!err.empty()) break;
        const Instr& last = ins[e - 1];
        std::string ln = last.name;
        size_t next = b + 1;
        if (jumped) line("break;");   // s_setpc / s_swappc set pc
        else if (last.fmt == Fmt::SOPP && ln == "s_endpgm" && open_block >= 0) line("// (the end: no block follows; the thread still makes the barrier calls below)");
        else if (last.fmt == Fmt::SOPP && ln == "s_endpgm") line(opt.raw_stats ? "{ dbg_add(1u); RETURN; }" : "RETURN;");
        else if (last.fmt == Fmt::SOPP && (starts(ln, "s_branch") || starts(ln, "s_cbranch"))) {
            size_t t = block_of.count(uint32_t(int32_t(last.offset) + 4 + last.simm16 * 4)) ? block_of[uint32_t(int32_t(last.offset) + 4 + last.simm16 * 4)] : nb;
            if (starts(ln, "s_branch")) line(loops ? fmt("pc = %zuu; break;", t) : fmt("reach[%zu] = true;", t));
            else {
                std::string cnd = branch_cond(last);
                if (loops && goesOn) { line(fmt("if (%s) { pc = %zuu; break; }", cnd.c_str(), t)); inCase = true; }
                else if (loops) line(fmt("pc = (%s) ? %zuu : %zuu; break;", cnd.c_str(), t, next));
                else line(fmt("if (%s) reach[%zu] = true; else reach[%zu] = true;", cnd.c_str(), t, next));
            }
        } else if (loops && goesOn) {
            inCase = true;
        } else {
            line(loops ? fmt("pc = %zuu; break;", next) : fmt("reach[%zu] = true;", next));
        }
        if (!inCase) line("}");
    }
    if (inCase) line("RETURN;\n  }");
    if (loops && coop) {
        // The count of threads with code left is a group-shared variable (XSL_LIVE). A program
        // whose own group memory is the 32 KB a PC allows has no room for one: there the last
        // dword of that memory is borrowed between two more barriers (the header decides, once
        // the size of the group memory is known: XSL_COOP_KEEP / XSL_COOP_RESTORE).
        line("default: { pc = 0xffffffffu; break; }\n  }\n  }\n  XSL_COOP_KEEP");
        line("if (gi_ == 0u) XSL_LIVE = 0u;\n  GroupMemoryBarrierWithGroupSync();");
        line("if (pc != 0xffffffffu) { uint o_; InterlockedAdd(XSL_LIVE, 1u, o_); }\n  GroupMemoryBarrierWithGroupSync();");
        line("bool none_ = XSL_LIVE == 0u;\n  XSL_COOP_RESTORE");
        line("if (none_) break;\n  atb_ = false;\n  }\n  return;");
    }
    else if (loops) line(opt.raw_stats ? "default: { dbg_add(2u); RETURN; }\n  }\n  }" : "default: RETURN;\n  }\n  }");
    else line("RETURN;");
    std::swap(out, body);
    if (!err.empty()) return "";

    std::ostringstream h;
    h << "// Translated by OpenScarlett gpucode from console GPU machine code.\n";
    h << "#define WAVE " << W << "u\n";
    if (dual) h << "#define XL_N 32u   // wave64 on wave32: cross-lane reads stay within a 32-lane half\n";
    for (auto& r : res)
        if (r.kind != ResKind::Counter && r.alias < 0) h << res_decl(r) << "\n";
    // Image descriptor words 1..4 (GFX10 T#) rebuilt from the bound texture for code that reads them:
    // width-1 [1]31:30 + [2]13:0, height-1 [2]29:14, base level [3]15:12, last level [3]19:16,
    // type [3]31:28, depth or last slice [4]12:0.
    for (auto& r : res) {
        if (!r.need_desc || r.alias >= 0) continue;
        int dim = r.dim < 0 ? 1 : r.dim;
        static const unsigned type[8] = {8, 9, 10, 11, 12, 13, 14, 15};
        h << "uint4 desc_" << r.name << "() {\n  uint w = 1u, h = 1u, d = 1u, l = 1u;\n";
        if (r.uav) h << (dim == 0 ? "  " + r.name + ".GetDimensions(w);\n" : dim == 2 || dim == 5 || dim == 3 ? "  " + r.name + ".GetDimensions(w, h, d);\n" : "  " + r.name + ".GetDimensions(w, h);\n");
        else if (dim == 6) h << "  " + r.name + ".GetDimensions(w, h, l); l = 1u;\n";   // multisampled: one level
        else if (dim == 7) h << "  " + r.name + ".GetDimensions(w, h, d, l); l = 1u;\n";
        else h << (dim == 0 ? "  " + r.name + ".GetDimensions(0u, w, l);\n" : dim == 2 || dim == 5 ? "  " + r.name + ".GetDimensions(0u, w, h, d, l);\n" : dim == 3 && r.cube_array ? "  " + r.name + ".GetDimensions(0u, w, h, d, l);\n" : "  " + r.name + ".GetDimensions(0u, w, h, l);\n");
        if (dim == 4) h << "  d = h; h = 1u;\n";
        h << fmt("  return uint4(((w - 1u) & 3u) << 30, ((w - 1u) >> 2) | ((h - 1u) << 14), ((l - 1u) << 16) | (%uu << 28), (d - 1u) & 0x1fffu);\n}\n", type[dim]);
    }
    // Root constants passed in user SGPRs.
    for (auto& a : meta.args) {
        if (a.kind != 0 || a.param >= rs.params.size()) continue;
        const RsParam& p = rs.params[a.param];
        if (p.type != ParamType::Constants) continue;
        h << fmt("cbuffer RC%u : register(b%u, space%u) { uint4 RC%u_d[%u]; };\n", a.param, p.reg, p.space, a.param, (p.num32 + 3) / 4);
    }
    unsigned ldsBytes = 0;
    if (uses_lds) {
        unsigned limit = c.kind == ProgramKind::MS ? 28672 : 32768;   // PC group-shared limits
        unsigned bytes = meta.lds_bytes() ? meta.lds_bytes() : c.kind == ProgramKind::MS && ms_lds ? std::min(limit, (ms_lds + 1023) & ~1023u) : limit;
        // Some code uses constant offsets past LDS_SIZE: cover the largest one (rounded to 1 KiB).
        unsigned need = 0;
        for (auto& in : ins)
            if (in.fmt == Fmt::DS && in.name) {
                std::string nm = in.name;
                unsigned o0 = uint32_t(in.offset_imm), o1 = uint32_t(in.gds_offset1);
                unsigned elem = has(nm, "b64") ? 8 : 4;
                bool two = has(nm, "read2") || has(nm, "write2") || has(nm, "wrxchg2");
                unsigned top = two ? std::max(o0, o1) * elem * (has(nm, "st64") ? 64 : 1) + elem : (o0 | o1 << 8) + 16;
                need = std::max(need, top);
            }
        if (need > bytes && need <= limit) bytes = std::min(limit, (need + 1023) & ~1023u);
        if (bytes > limit) return fail(fmt("LDS size %u exceeds the PC group-shared limit", bytes)), "";
        h << "groupshared uint lds[" << bytes / 4 << "];\n";
        ldsBytes = bytes;
    }
    if (coop) {   // (see the end of the dispatcher)
        if (ldsBytes + 4 > 32768) {
            const unsigned last = ldsBytes / 4 - 1;
            h << fmt("#define XSL_LIVE lds[%uu]\n", last);
            h << fmt("#define XSL_COOP_KEEP uint keep_ = 0u; GroupMemoryBarrierWithGroupSync(); if (gi_ == 0u) keep_ = lds[%uu];\n", last);
            h << fmt("#define XSL_COOP_RESTORE GroupMemoryBarrierWithGroupSync(); if (gi_ == 0u) lds[%uu] = keep_; GroupMemoryBarrierWithGroupSync();\n", last);
        } else h << "groupshared uint gs_live_;\n#define XSL_LIVE gs_live_\n#define XSL_COOP_KEEP\n#define XSL_COOP_RESTORE\n";
    }
    h << kPrelude;
    if (uses_quad) h << kQuadPrelude;
    const std::string emitted = body.str();
    if (uses_dot4 || has(emitted, "dot4_i8(")) {   // (the two-source form is in the table of simple operations)
        uses_dot4 = true;
        if (opt.dot4_intrinsic) h << "uint dot4_i8(uint a, uint b, uint c) { return uint(dot4add_i8packed(a, b, int(c))); }\n";
        else h << "uint dot4_i8(uint a, uint b, uint c) {\n"
                  "  int4 x = int4(int(a << 24) >> 24, int(a << 16) >> 24, int(a << 8) >> 24, int(a) >> 24);\n"
                  "  int4 y = int4(int(b << 24) >> 24, int(b << 16) >> 24, int(b << 8) >> 24, int(b) >> 24);\n"
                  "  return uint(x.x * y.x + x.y * y.y + x.z * y.z + x.w * y.w) + c;\n}\n";
    }
    if (has(emitted, "pk_fmac("))
        h << "uint pk_fmac(uint a, uint b, uint d) {   // packed f16: d + a * b in each half\n"
             "  return (f16r(f16tof32(a) * f16tof32(b) + f16tof32(d)) & 0xffffu) | (f16r(f16tof32(a >> 16) * f16tof32(b >> 16) + f16tof32(d >> 16)) << 16);\n}\n";
    if (uses_gm && opt.global_memory_test) {
        h << "ByteAddressBuffer GMTEST : register(t1, space0);   // tests: global memory = this buffer\n"
             "uint gm_load(uint lo, uint hi) { uint n_; GMTEST.GetDimensions(n_); return hi == 0u && lo + 4u <= n_ ? GMTEST.Load(lo & ~3u) : 0u; }\n"
             "void gm_store(uint lo, uint hi, uint v) {}\n"
             "uint gm_atomic(uint lo, uint hi, uint op, uint v) { return 0u; }\n"
             "uint gm_cmpswap(uint lo, uint hi, uint cmp, uint v) { return 0u; }\n"
             "uint gm_atomic_f(uint lo, uint hi, bool mx, uint v) { return 0u; }\n"
             "void gm_store_bits(uint lo, uint hi, uint v, uint mask) {}\n";
    } else if (uses_gm) {
        h << "StructuredBuffer<uint4> GMT : register(t0, space901);\n"
             "ByteAddressBuffer GMS[8191] : register(t0, space900);\n"
             "RWByteAddressBuffer GMU[8192] : register(u0, space900);\n"
             "// Entry of the address table holding (lo, hi): found, view word, byte offset. A fixed-step\n"
             "// search (tables up to 32767 entries) without branches or loops (inlined at every access; loops there made DXC superlinear).\n"
             "bool gm_find(uint lo, uint hi, out uint view, out uint off, out uint left) {\n"
             "  uint a = 0u, n = GMT[0].x, t, ex, ey;\n"
             "  t = a + 16384u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 8192u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 4096u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 2048u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 1024u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 512u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 256u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 128u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 64u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 32u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 16u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 8u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 4u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 2u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  t = a + 1u; ex = GMT[min(t, n)].x; ey = GMT[min(t, n)].y; a = select((t <= n) & ((ey < hi) | ((ey == hi) & (ex <= lo))), t, a);\n"
             "  ex = GMT[a].x; ey = GMT[a].y; uint ez = GMT[a].z; view = GMT[a].w;\n"
             "  off = lo - ex; uint ohi = hi - ey - select(lo < ex, 1u, 0u);\n"
             "  left = ez - off;\n"
             "  return (a != 0u) & (ohi == 0u) & (off < ez);\n"
             "}\n"
             "// A dword of a view gm_find returned (several dwords of one access share one search).\n"
             "uint gm_vload(uint fy, uint off) {\n"
             "  if (fy & 0x40000000u) return GMS[NonUniformResourceIndex(fy & 0x3fffu)].Load(off & ~3u);\n"
             "  if (fy & 0x80000000u) return GMU[NonUniformResourceIndex((fy >> 14) & 0x3fffu)].Load(off & ~3u);\n"
             "  return 0u;\n"
             "}\n"
             "uint gm_load(uint lo, uint hi) {\n"
             "  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n"
             "  if (!fx) return 0u;\n"
             "  if (fy & 0x40000000u) return GMS[NonUniformResourceIndex(fy & 0x3fffu)].Load(fz & ~3u);\n"
             "  if (fy & 0x80000000u) return GMU[NonUniformResourceIndex((fy >> 14) & 0x3fffu)].Load(fz & ~3u);\n"
             "  return 0u;\n"
             "}\n"
             "void gm_store(uint lo, uint hi, uint v) {\n"
             "  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n"
             "  if (fx && (fy & 0x80000000u)) GMU[NonUniformResourceIndex((fy >> 14) & 0x3fffu)].Store(fz & ~3u, v);\n"
             "}\n"
             "uint gm_atomic(uint lo, uint hi, uint op, uint v) {   // 1 add 2 sub 3 umin 4 umax 5 smin 6 smax 7 and 8 or 9 xor 10 swap\n"
             "  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n"
             "  if (!fx || !(fy & 0x80000000u)) return 0u;\n"
             "  uint i = (fy >> 14) & 0x3fffu, a = fz & ~3u, o = 0u; int so = 0;\n"
             "  switch (op) {\n"
             "  case 1u: GMU[NonUniformResourceIndex(i)].InterlockedAdd(a, v, o); break;\n"
             "  case 2u: GMU[NonUniformResourceIndex(i)].InterlockedAdd(a, 0u - v, o); break;\n"
             "  case 3u: GMU[NonUniformResourceIndex(i)].InterlockedMin(a, v, o); break;\n"
             "  case 4u: GMU[NonUniformResourceIndex(i)].InterlockedMax(a, v, o); break;\n"
             "  case 5u: GMU[NonUniformResourceIndex(i)].InterlockedMin(a, int(v), so); o = uint(so); break;\n"
             "  case 6u: GMU[NonUniformResourceIndex(i)].InterlockedMax(a, int(v), so); o = uint(so); break;\n"
             "  case 7u: GMU[NonUniformResourceIndex(i)].InterlockedAnd(a, v, o); break;\n"
             "  case 8u: GMU[NonUniformResourceIndex(i)].InterlockedOr(a, v, o); break;\n"
             "  case 9u: GMU[NonUniformResourceIndex(i)].InterlockedXor(a, v, o); break;\n"
             "  default: GMU[NonUniformResourceIndex(i)].InterlockedExchange(a, v, o); break;\n"
             "  }\n  return o;\n}\n"
             "uint gm_cmpswap(uint lo, uint hi, uint cmp, uint v) {\n"
             "  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n"
             "  if (!fx || !(fy & 0x80000000u)) return 0u;\n"
             "  uint o = 0u; GMU[NonUniformResourceIndex((fy >> 14) & 0x3fffu)].InterlockedCompareExchange(fz & ~3u, cmp, v, o);\n"
             "  return o;\n}\n"
             "// Float minimum / maximum: stored when the operand compares below / above what is there\n"
             "// (never for a NaN). Another thread's store in between is retried; each retry sees a value\n"
             "// nearer the operand, so a few rounds settle it.\n"
             "uint gm_atomic_f(uint lo, uint hi, bool mx, uint v) {\n"
             "  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n"
             "  if (!fx || !(fy & 0x80000000u)) return 0u;\n"
             "  uint i = (fy >> 14) & 0x3fffu, a = fz & ~3u;\n"
             "  uint cur = GMU[NonUniformResourceIndex(i)].Load(a);\n"
             "  for (uint k = 0u; k < 24u; k++) {\n"
             "    bool take = mx ? asfloat(v) > asfloat(cur) : asfloat(v) < asfloat(cur);\n"
             "    if (!take) break;\n"
             "    uint o = 0u; GMU[NonUniformResourceIndex(i)].InterlockedCompareExchange(a, cur, v, o);\n"
             "    if (o == cur) break;\n"
             "    cur = o;\n"
             "  }\n  return cur;\n}\n"
             "void gm_store_bits(uint lo, uint hi, uint v, uint mask) {   // byte/short stores: atomic clear and set\n"
             "  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n"
             "  if (!fx || !(fy & 0x80000000u)) return;\n"
             "  uint s = (fz & 3u) * 8u, o;\n"
             "  GMU[NonUniformResourceIndex((fy >> 14) & 0x3fffu)].InterlockedAnd(fz & ~3u, ~(mask << s), o);\n"
             "  GMU[NonUniformResourceIndex((fy >> 14) & 0x3fffu)].InterlockedOr(fz & ~3u, v << s, o);\n"
             "}\n";
    }
    if (uses_bvh) h << kBvhPrelude;
    if (uses_fmt_comp) {
        // Component k of the element at a global address, for the GFX10 buffer formats with 8/16/32-bit
        // components (float results as bits; missing components 0, alpha 1).
        h << "uint fmt_bits(uint lo, uint hi, uint bit, uint size) {\n"
             "  uint a = lo + (bit >> 3), w = gm_load(a & ~3u, hi + (a < lo ? 1u : 0u)) >> ((a & 3u) * 8u);\n"
             "  return size >= 32u ? w : w & ((1u << size) - 1u);\n}\n";
        // Table-driven (one small code path, not a switch over formats: it is inlined at every
        // typed load and the switch made programs slow to compile): entry = component size |
        // count << 8 | type << 12 (0 uint, 1 sint, 2 float, 3 unorm, 4 snorm, 5 uscaled, 6 sscaled).
        uint32_t tbl[128] = {};
        for (auto& fm : kBufFmts) {
            std::string nm = fm.name;
            std::vector<unsigned> sz;
            size_t p0 = 0;
            while (p0 < nm.size() && isdigit((unsigned char)nm[p0])) {
                size_t q = nm.find('_', p0);
                sz.push_back((unsigned)atoi(nm.substr(p0, q - p0).c_str()));
                p0 = q + 1;
            }
            std::string ty = nm.substr(p0);
            static const char* types[] = {"UINT", "SINT", "FLOAT", "UNORM", "SNORM", "USCALED", "SSCALED"};
            int t = -1;
            for (int k = 0; k < 7; k++)
                if (ty == types[k]) t = k;
            bool ok = !sz.empty() && t >= 0 && fm.id < 128;
            for (unsigned z : sz) ok &= (z == 8 || z == 16 || z == 32) && z == sz[0];
            if (t == 2) for (unsigned z : sz) ok &= z == 16 || z == 32;
            if (ok) tbl[fm.id] = sz[0] | uint32_t(sz.size()) << 8 | uint32_t(t) << 12;
        }
        h << "static const uint FMTD[128] = {";
        for (int k = 0; k < 128; k++) h << (k ? ", " : "") << fmt("0x%xu", tbl[k]);
        h << "};\n";
        h << "uint fmt_comp(uint lo, uint hi, uint f, uint k) {\n"
             "  uint d = FMTD[f & 127u], z = d & 0xffu, n = (d >> 8) & 15u, t = d >> 12;\n"
             "  if (d == 0u) return 0u;\n"
             "  if (k >= n) return k == 3u ? (t >= 2u ? 0x3f800000u : 1u) : 0u;\n"
             "  uint v = fmt_bits(lo, hi, k * z, z), sh = 32u - z;\n"
             "  int sv = z == 32u ? int(v) : (int(v << sh) >> sh);\n"
             "  float maxu = float((z == 32u ? 0xffffffffu : (1u << z) - 1u)), maxs = float((1u << (z - 1u)) - 1u);\n"
             "  if (t == 0u) return v;\n"
             "  if (t == 1u) return uint(sv);\n"
             "  if (t == 2u) return z == 32u ? v : asuint(f16tof32(v));\n"
             "  if (t == 3u) return asuint(float(v) / maxu);\n"
             "  if (t == 4u) return asuint(max(float(sv) / maxs, -1.0));\n"
             "  if (t == 5u) return asuint(float(v));\n"
             "  return asuint(float(sv));\n"
             "}\n";
        // The other way: component k (float results as bits) encoded and written. Components the
        // format does not have are not stored.
        if (uses_fmt_put)
        h << "void fmt_put(uint lo, uint hi, uint f, uint k, uint v) {\n"
             "  uint d = FMTD[f & 127u], z = d & 0xffu, n = (d >> 8) & 15u, t = d >> 12;\n"
             "  if (d == 0u || k >= n) return;\n"
             "  uint mask = z == 32u ? 0xffffffffu : (1u << z) - 1u, e = v;\n"
             "  float maxu = float(mask), maxs = float((1u << (z - 1u)) - 1u), x = asfloat(v);\n"
             "  if (t == 2u) e = z == 32u ? v : f32tof16(x);\n"
             "  else if (t == 3u) e = uint(round(saturate(x) * maxu));\n"
             "  else if (t == 4u) e = uint(int(round(clamp(x, -1.0, 1.0) * maxs)));\n"
             "  else if (t == 5u) e = uint(max(x, 0.0));\n"
             "  else if (t == 6u) e = uint(int(x));\n"
             "  uint a = lo + k * (z >> 3), ah = hi + (a < lo ? 1u : 0u);\n"
             "  if (z == 32u) gm_store(a, ah, e); else gm_store_bits(a, ah, e & mask, mask);\n"
             "}\n";
    }
    {   // bindless global-heap arrays for image access through descriptor memory (spaces 910+)
        unsigned sp = 910;
        for (unsigned d : {1u, 5u, 2u, 0u}) {   // spaces 910-918, and (past the cubes and samplers) 1D textures in 921-923
            const char* tag = d == 1 ? "2" : d == 5 ? "2A" : d == 0 ? "1" : "3";
            const char* ty = d == 1 ? "Texture2D" : d == 5 ? "Texture2DArray" : d == 0 ? "Texture1D" : "Texture3D";
            if (d == 0) sp = 921;
            if (heap_dims.count(d)) {
                h << fmt("RW%s<float4> HU%sF[] : register(u0, space%u);\nRW%s<uint4> HU%sU[] : register(u0, space%u);\nRW%s<int4> HU%sI[] : register(u0, space%u);\n",
                         ty, tag, sp, ty, tag, sp + 1, ty, tag, sp + 2);
                h << fmt("%s<float4> HS%sF[] : register(t0, space%u);\n%s<uint4> HS%sU[] : register(t0, space%u);\n%s<int4> HS%sI[] : register(t0, space%u);\n",
                         ty, tag, sp, ty, tag, sp + 1, ty, tag, sp + 2);
            }
            sp += 3;
        }
        if (uses_heap_cube) h << "TextureCube<float4> HSCF[] : register(t0, space919);\n";
        if (uses_heap_sampler) h << "SamplerState HSMP[] : register(s0, space920);\n";
        if (uses_heap_sampler_cmp) h << "SamplerComparisonState HSMPC[] : register(s0, space925);\n";
        if (uses_park) h << "RWByteAddressBuffer RTST : register(u0, space930);\nRWByteAddressBuffer RTDISC : register(u0, space931);\n";
    }
    if (indirect) {   // code address -> dispatcher case (block start); unknown addresses end the invocation
        // Sorted (hi, lo) table searched in fixed branch-free steps: a switch over every block was
        // inlined at each indirect jump and made programs very slow for the driver to compile.
        std::vector<std::pair<uint64_t, size_t>> tab;
        for (size_t b = 0; b < block_start.size(); b++) tab.push_back({addr_of(ins[block_start[b]].offset), b});
        std::sort(tab.begin(), tab.end());
        size_t n = tab.size();
        h << fmt("static const uint JT_HI[%zu] = {", n);
        for (size_t k = 0; k < n; k++) h << (k ? ", " : "") << fmt("0x%xu", uint32_t(tab[k].first >> 32));
        h << "};\n" << fmt("static const uint JT_LO[%zu] = {", n);
        for (size_t k = 0; k < n; k++) h << (k ? ", " : "") << fmt("0x%xu", uint32_t(tab[k].first));
        h << "};\n" << fmt("static const uint JT_B[%zu] = {", n);
        for (size_t k = 0; k < n; k++) h << (k ? ", " : "") << fmt("%zuu", tab[k].second);
        h << "};\n";
        unsigned top = 1;
        while (top * 2 <= n) top *= 2;
        h << "uint jump_case(uint lo, uint hi) {\n  uint a = 0u, t;\n";   // a = count of entries <= (hi, lo)
        for (unsigned st = top; st; st >>= 1)
            h << fmt("  t = a + %uu; a = select((t <= %zuu) & ((JT_HI[min(t, %zuu) - 1u] < hi) | ((JT_HI[min(t, %zuu) - 1u] == hi) & (JT_LO[min(t, %zuu) - 1u] <= lo))), t, a);\n",
                     st, n, n, n, n);
        h << "  if (a == 0u) return 0xffffffffu;\n";
        h << "  return (JT_HI[a - 1u] == hi && JT_LO[a - 1u] == lo) ? JT_B[a - 1u] : 0xffffffffu;\n}\n";
    }
    if (uses_codedata) {
        size_t n = (c.precompiled.size() - c.body_begin) / 4;
        h << "static const uint codedata[" << n << "] = {";
        for (size_t i = 0; i < n; i++) {
            uint32_t v;
            memcpy(&v, c.precompiled.data() + c.body_begin + 4 * i, 4);
            h << (i ? "," : "") << fmt("0x%xu", v);
        }
        h << "};\n#define CODEDATA_N " << n << "u\n";
    }
    auto type_of = [](const SigElem& e) {
        unsigned w = unsigned(__builtin_popcount(e.mask));
        const char* b = e.base_type();
        return w > 1 ? fmt("%s%u", b, w) : std::string(b);
    };
    auto sem = [](const SigElem& e) { return e.index ? fmt("%s%u", e.name.c_str(), e.index) : e.name; };
    auto sorted = [](std::vector<SigElem> v) {
        std::sort(v.begin(), v.end(), [](const SigElem& a, const SigElem& b) { return a.reg != b.reg ? a.reg < b.reg : (a.mask & -a.mask) < (b.mask & -b.mask); });
        return v;
    };
    std::string init;   // stage-specific initialisation of registers
    if (c.kind == ProgramKind::CS && dual) {
        // PC thread gi_ (32-lane waves) runs console threads t0 = (gi_/32)*64 + gi_%32 and t0 + 32
        // of the console group (flattened x-fastest, as the console packs waves).
        unsigned nx = meta.numthreads[0], ny = meta.numthreads[1], nt = nx * ny * meta.numthreads[2];
        h << (coop ? "#define RETURN { pc = 0xffffffffu; break; }\n" : "#define RETURN return\n");
        h << "\n" << (opt.wave_size_attr ? "[WaveSize(32)]\n" : "") << "[numthreads(" << nt / 2 << ", 1, 1)]\n";
        h << "void main(uint3 gid_ : SV_GroupID, uint gi_ : SV_GroupIndex) {\n";
        unsigned s = meta.user_sgprs();
        for (int k = 0; k < 3; k++)
            if (meta.tgid_en(k)) init += fmt("  s%u = gid_.%c;\n", s++, "xyz"[k]);
        if (meta.tg_size_en()) init += fmt("  s%u = ((gi_ / 32u) << 6) | %uu;   // TG_SIZE: console wave id, wave count\n", s++, (nt + 63) / 64);
        init += "  uint t0_ = (gi_ / 32u) * 64u + gi_ % 32u;\n";
        for (int cc = 0; cc < 2; cc++) {
            std::string t = cc ? "(t0_ + 32u)" : "t0_";
            for (unsigned k = 0; k < meta.tid_comps(); k++) {
                std::string ex = k == 0 ? fmt("%s %% %uu", t.c_str(), nx) : k == 1 ? fmt("(%s / %uu) %% %uu", t.c_str(), nx, ny) : fmt("%s / %uu", t.c_str(), nx * ny);
                init += fmt("  v%u_%d = %s;\n", k, cc, ex.c_str());
            }
        }
        max_v = std::max(max_v, meta.tid_comps());
    } else if (c.kind == ProgramKind::CS) {
        h << (coop ? "#define RETURN { pc = 0xffffffffu; break; }\n" : "#define RETURN return\n");
        h << "\n[numthreads(" << meta.numthreads[0] << ", " << meta.numthreads[1] << ", " << meta.numthreads[2] << ")]\n";
        h << "void main(uint3 gid_ : SV_GroupID, uint3 tid_ : SV_GroupThreadID, uint gi_ : SV_GroupIndex) {\n";
        unsigned s = meta.user_sgprs();
        for (int k = 0; k < 3; k++)
            if (meta.tgid_en(k)) init += fmt("  s%u = gid_.%c;\n", s++, "xyz"[k]);
        if (meta.tg_size_en()) {
            unsigned nt = meta.numthreads[0] * meta.numthreads[1] * meta.numthreads[2];
            init += fmt("  s%u = ((gi_ / WAVE) << 6) | %uu;   // TG_SIZE: wave id in group, wave count\n", s++, (nt + W - 1) / W);
        }
        if ((meta.rsrc2 & 1) && !opt.raw_scratch_offset.empty())   // SCRATCH_EN: the scratch wave offset follows
            init += fmt("  s%u = %s;   // scratch wave offset\n", s++, opt.raw_scratch_offset.c_str());
        if (opt.raw_stats) init += "  DBGB = RC0_d[1].y; dbg_add(0u);\n";
        if (opt.raw_park)   // most waves of most programs have nothing to do in the rounds after the first
            init += fmt("  if ((RC0_d[1].z & 0xffu) != 0u) {   // later rounds: only waves with a lane parked on this program's code run\n"
                        "    uint l_ = %s; bool r_ = false;\n"
                        "    if (l_ < RC0_d[1].w) { uint2 h_ = RTST.Load2(l_ * %uu); r_ = h_.y == RC0_d[1].z && jump_case(h_.x << 8, h_.x >> 24) != 0xffffffffu; }\n"
                        "    if (!WaveActiveAnyTrue(r_)) return;\n  }\n", park_lane().c_str(), 8 + 4 * opt.raw_park_vgprs);
        for (unsigned k = 0; k < meta.tid_comps(); k++) init += fmt("  v%u = tid_.%c;\n", k, "xyz"[k]);
    } else if (c.kind == ProgramKind::PS) {
        h << "struct PSIn {\n";
        bool has_pos = false, has_face = false, has_sid = false, has_cov = false;
        // Values the rasterizer generates (front face, sample index, primitive id) go after the
        // ones the previous stage writes, so the PC signature packs like the previous stage's output.
        std::string tail;
        for (auto& e : sorted(isg)) {
            if (e.sysval == 1) { h << "  float4 pos_ : SV_Position;\n"; has_pos = true; continue; }
            if (e.sysval == 9) { tail += "  bool face_ : SV_IsFrontFace;\n"; has_face = true; continue; }
            if (e.sysval == 10) { tail += "  uint sid_ : SV_SampleIndex;\n"; has_sid = true; continue; }
            if (e.sysval == 7) { tail += "  " + type_of(e) + " sv" + std::to_string(e.reg) + "_ : " + sem(e) + ";\n"; continue; }
            if (e.sysval != 0) { h << "  " << type_of(e) << " sv" << e.reg << "_ : " << sem(e) << ";\n"; continue; }
            unsigned first = unsigned(__builtin_ctz(e.mask));
            auto m = attr_mode.find(e.reg * 4 + first);
            std::string md = m != attr_mode.end() ? m->second : (e.is_float() ? "linear" : "nointerpolation");
            // Components sharing a register share its first element's mode: the PC compiler packs
            // differently interpolated values into separate rows, which would no longer line up
            // with the previous stage's output registers.
            for (auto& o : isg)
                if (o.reg == e.reg && o.sysval == 0 && (o.mask & -o.mask) < (e.mask & -e.mask) && o.is_float() == e.is_float()) {
                    auto mo = attr_mode.find(o.reg * 4 + unsigned(__builtin_ctz(o.mask)));
                    md = mo != attr_mode.end() ? mo->second : (o.is_float() ? "linear" : "nointerpolation");
                    break;
                }
            if (md == "linear") md = "";
            h << "  " << md << (md.empty() ? "" : " ") << type_of(e) << " a" << e.reg << "_" << first << " : " << sem(e) << ";\n";
        }
        for (auto& pr : ps_sysv) {
            const std::string& ini = pr.second;
            if (has(ini, "pos_") && !has_pos) { h << "  float4 pos_ : SV_Position;\n"; has_pos = true; }
            if (has(ini, "face_") && !has_face) { tail += "  bool face_ : SV_IsFrontFace;\n"; has_face = true; }
            if (has(ini, "sid_") && !has_sid) { tail += "  uint sid_ : SV_SampleIndex;\n"; has_sid = true; }
            if (has(ini, "cov_") && !has_cov) { tail += "  uint cov_ : SV_Coverage;\n"; has_cov = true; }
        }
        h << tail;
        h << "};\nstruct PSOut {\n";
        for (unsigned t : ps_targets) {
            bool isint = false, sgn = false;
            for (auto& e : osg)
                if (e.sysval == 64 && e.index == t) { isint = !e.is_float(); sgn = e.is_sint(); }
            h << "  " << (isint ? (sgn ? "int4" : "uint4") : "float4") << " t" << t << " : SV_Target" << t << ";\n";
        }
        if (ps_depth) {
            std::string dn = "SV_Depth";
            for (auto& e : osg)
                if (starts(e.name, "SV_Depth")) dn = e.name;
            h << "  float depth_ : " << dn << ";\n";
        }
        if (ps_stencil) h << "  uint stencil_ : SV_StencilRef;\n";
        if (ps_mask) h << "  uint mask_ : SV_Coverage;\n";
        if (ps_targets.empty() && !ps_depth && !ps_stencil && !ps_mask) h << "  float4 t0 : SV_Target0;\n";
        h << "};\n#define RETURN return O\n";
        h << "\nPSOut main(PSIn I) {\n  PSOut O = (PSOut)0;\n";
        for (auto& pr : ps_sysv) init += fmt("  v%u = %s;\n", pr.first, pr.second.c_str());
    } else if (c.kind == ProgramKind::VS) {
        h << "struct VSIn {\n";
        unsigned vg = 4;
        bool vid = false, iid = false;
        for (auto& e : sorted(isg)) {
            if (e.sysval == 6) { h << "  uint vid_ : SV_VertexID;\n"; vid = true; continue; }
            if (e.sysval == 8) { h << "  uint iid_ : SV_InstanceID;\n"; iid = true; continue; }
            if (e.sysval != 0) continue;
            h << "  " << type_of(e) << " i" << e.reg << " : " << sem(e) << ";\n";
            unsigned w = unsigned(__builtin_popcount(e.mask));
            if (vs_fetch) {
                // The fetch shader leaves each attribute in 4 VGPRs from v4, filling missing
                // components with (0, 0, 0, 1).
                for (unsigned k = 0; k < 4; k++) {
                    std::string f = k >= w ? (k == 3 ? (e.is_float() ? "1.0f" : "1") : "0") : w > 1 ? fmt("I.i%u.%c", e.reg, "xyzw"[k]) : fmt("I.i%u", e.reg);
                    init += fmt("  v%u = %s;\n", vg + k, e.is_float() ? ("asuint(" + f + ")").c_str() : ("uint(" + f + ")").c_str());
                }
                max_v = std::max(max_v, vg + 4);
                vg += 4;
            }
        }
        if (!vid) h << "  uint vid_ : SV_VertexID;\n";
        if (!iid) h << "  uint iid_ : SV_InstanceID;\n";
        h << "};\nstruct VSOut {\n";
        bool pos_in_sig = false;
        for (auto& e : sorted(osg)) {
            if (e.sysval == 1) { h << "  float4 pos_ : SV_Position;\n"; pos_in_sig = true; continue; }
            if (e.sysval != 0) continue;
            unsigned w = unsigned(__builtin_popcount(e.mask));
            const char* b = e.base_type();
            h << "  " << (w > 1 ? fmt("%s%u", b, w) : std::string(b)) << " o" << e.reg << "_" << __builtin_ctz(e.mask) << " : " << sem(e) << ";\n";
        }
        if (!pos_in_sig) h << "  float4 pos_ : SV_Position;\n";
        h << sysv_outputs(sorted(osg));
        h << "};\n";
        if (uses_clipcull) h << "static float clipcull_[8];\n#define RETURN { " << clipcull_copy() << "return O; }\n";
        else h << "#define RETURN return O\n";
        h << "\nVSOut main(VSIn I) {\n  VSOut O = (VSOut)0;\n";
        init += "  v0 = I.vid_;\n";
        init += "  v3 = I.iid_;\n";   // with or without a fetch-shader call (it fills v4 up and keeps v0..v3)
    }
    if (c.kind == ProgramKind::MS) {
        // The program runs as prog(); each thread captures the vertex and primitive it exports
        // (thread i owns output slot i) and the counts sent with MSG_GS_ALLOC_REQ; main() then sets
        // the output counts in uniform control flow and writes the captured outputs.
        h << "struct MSV {\n";
        bool pos_in_sig = false;
        for (auto& e : sorted(osg)) {
            if (e.sysval == 1) { h << "  float4 pos_ : SV_Position;\n"; pos_in_sig = true; continue; }
            if (e.sysval != 0) continue;
            unsigned w = unsigned(__builtin_popcount(e.mask));
            const char* b = e.base_type();
            h << "  " << (w > 1 ? fmt("%s%u", b, w) : std::string(b)) << " o" << e.reg << "_" << __builtin_ctz(e.mask) << " : " << sem(e) << ";\n";
        }
        if (!pos_in_sig) h << "  float4 pos_ : SV_Position;\n";
        h << sysv_outputs(sorted(osg));
        h << "};\n";
        if (has_prims()) {
            // Per-primitive outputs (system values other than these are left at their defaults).
            h << "struct MSP {\n";
            for (auto& e : sorted(psg)) {
                if (e.sysval != 0) continue;
                unsigned w = unsigned(__builtin_popcount(e.mask));
                const char* b = e.base_type();
                h << "  " << (w > 1 ? fmt("%s%u", b, w) : std::string(b)) << " p" << e.reg << "_" << __builtin_ctz(e.mask) << " : " << sem(e) << ";\n";
            }
            h << "};\n" << (dual ? "static MSP P_0, P_1;\n" : "static MSP P;\n");
        }
        h << (uses_clipcull ? "static float clipcull_[8];\n" : "");
        unsigned nt = meta.numthreads[0] * meta.numthreads[1] * meta.numthreads[2];
        if (dual) {
            h << "groupshared uint ms_counts_;\nstatic MSV O_0, O_1;\nstatic uint prim__0, prim__1;\nstatic bool hasv__0, hasv__1, hasp__0, hasp__1;\n";
            if (uses_clipcull) h << "static float clipcull__0[8], clipcull__1[8];\n";
            h << "#define RETURN return\n\nvoid prog(uint3 gid_, uint gi_) {\n";
            // PC thread gi_ (32-lane waves) runs console threads t0 = (gi_/32)*64 + gi_%32 and t0 + 32.
            init += fmt("  s3 = ((gi_ / 32u) << 24) | min(64u, %uu - (gi_ / 32u) * 64u);   // console wave id, threads in wave\n", nt);
            init += fmt("  { uint t0_ = (gi_ / 32u) * 64u + gi_ %% 32u; v5_0 = gid_.x * %uu + t0_; v5_1 = v5_0 + 32u; }\n", nt);
        } else {
            h << "groupshared uint ms_counts_;\nstatic MSV O;\nstatic uint prim_;\nstatic bool hasv_, hasp_;\n#define RETURN return\n";
            h << "\nvoid prog(uint3 gid_, uint gi_) {\n";
            init += fmt("  s3 = ((gi_ / WAVE) << 24) | min(WAVE, %uu - (gi_ / WAVE) * WAVE);   // wave id, threads in wave\n", nt);
            init += fmt("  v5 = gid_.x * %uu + gi_;   // flat thread id in the dispatch\n", nt);
        }
    }
    if (dual) h << "  uint LP_ = WaveGetLaneIndex();   // PC lane; console lanes LP_ and LP_ + 32\n  L = LP_; LG = L;\n";
    else {
        h << "  L = WaveGetLaneIndex();\n";
        h << (c.kind == ProgramKind::CS || c.kind == ProgramKind::MS ? "  LG = gi_ % WAVE;\n" : "  LG = L;\n");
    }
    h << "  uint snull = 0u;\n";
    for (unsigned i = 0; i < kNumS; i++) h << (i % 16 == 0 ? "  uint " : ", ") << "s" << i << " = 0u" << (i % 16 == 15 ? ";\n" : "");
    const unsigned nv = std::max(max_v, 6u);
    for (int c = 0; c < (dual ? 2 : 1); c++)
        for (unsigned i = 0; i < nv; i++)
            h << (i % 16 == 0 ? "  uint " : ", ") << "v" << i << (dual ? fmt("_%d", c) : std::string()) << " = 0u" << (i % 16 == 15 || i + 1 == nv ? ";\n" : "");
    h << (dual ? "  bool scc = false, kexec_0 = true, kexec_1 = true, kvcc_0 = false, kvcc_1 = false;\n" : "  bool scc = false, kexec = true, kvcc = false;\n");
    for (size_t d : dyn_loads) h << "  uint dyn" << d << " = 0u;\n";
    for (auto& a : meta.args) {
        if (a.kind != 0 || a.loc >= 32 || a.param >= rs.params.size() || rs.params[a.param].type != ParamType::Constants) continue;
        for (unsigned k = 0; k < a.size; k++) h << fmt("  s%u = RC%u_d[%u][%u];\n", a.loc + k, a.param, k / 4, k % 4);
    }
    if (dual) h << "  s126 = ballot2(true).x; s127 = s126;\n";
    else h << "  { uint2 b_ = ballot2(true); s126 = b_.x; s127 = " << (W == 64 ? "b_.y" : "0u") << "; }\n";
    h << init;
    h << expand_movrel(body.str(), nv);
    h << "}\n";
    if (c.kind == ProgramKind::MS && dual) {
        unsigned nt = meta.numthreads[0] * meta.numthreads[1] * meta.numthreads[2];
        // Mesh shaders cannot request a wave size; the dual mapping assumes 32-lane waves (NVIDIA;
        // other GPUs choose their mesh wave size).
        h << "\n[outputtopology(\"triangle\")]\n[numthreads(" << nt / 2 << ", 1, 1)]\n";
        h << "void main(uint gi_ : SV_GroupIndex, uint3 gid_ : SV_GroupID, out vertices MSV verts[" << ms_max_verts << "], out indices uint3 tris[" << ms_max_prims << "]" << (has_prims() ? fmt(", out primitives MSP prims[%u]", ms_max_prims) : std::string()) << ") {\n";
        h << "  if (gi_ == 0u) ms_counts_ = 0u;\n  GroupMemoryBarrierWithGroupSync();\n";
        if (has_prims()) h << "  P_0 = (MSP)0; P_1 = (MSP)0;\n";
        h << "  O_0 = (MSV)0; O_1 = (MSV)0; prim__0 = 0u; prim__1 = 0u; hasp__0 = false; hasp__1 = false; hasv__0 = false; hasv__1 = false;\n";
        h << "  prog(gid_, gi_);\n  GroupMemoryBarrierWithGroupSync();\n";
        h << "  uint nv = min(ms_counts_ & 0xfffu, " << ms_max_verts << "u), np = min(ms_counts_ >> 12, " << ms_max_prims << "u);\n";
        h << "  SetMeshOutputCounts(nv, np);\n";
        h << "  uint t0 = (gi_ / 32u) * 64u + gi_ % 32u, t1 = t0 + 32u;\n";
        h << "  if (t0 < nv) verts[t0] = O_0;\n  if (t1 < nv) verts[t1] = O_1;\n";
        if (has_prims()) h << "  if (t0 < np) prims[t0] = P_0;\n  if (t1 < np) prims[t1] = P_1;\n";
        h << "  if (t0 < np) tris[t0] = (hasp__0 && (prim__0 >> 31) == 0u) ? uint3(prim__0 & 0x1ffu, (prim__0 >> 10) & 0x1ffu, (prim__0 >> 20) & 0x1ffu) : uint3(0u, 0u, 0u);\n";
        h << "  if (t1 < np) tris[t1] = (hasp__1 && (prim__1 >> 31) == 0u) ? uint3(prim__1 & 0x1ffu, (prim__1 >> 10) & 0x1ffu, (prim__1 >> 20) & 0x1ffu) : uint3(0u, 0u, 0u);\n";
        h << "}\n";
    } else if (c.kind == ProgramKind::MS) {
        h << "\n[outputtopology(\"triangle\")]\n[numthreads(" << meta.numthreads[0] << ", " << meta.numthreads[1] << ", " << meta.numthreads[2] << ")]\n";
        h << "void main(uint gi_ : SV_GroupIndex, uint3 gid_ : SV_GroupID, out vertices MSV verts[" << ms_max_verts << "], out indices uint3 tris[" << ms_max_prims << "]" << (has_prims() ? fmt(", out primitives MSP prims[%u]", ms_max_prims) : std::string()) << ") {\n";
        h << "  if (gi_ == 0u) ms_counts_ = 0u;\n  GroupMemoryBarrierWithGroupSync();\n";
        if (has_prims()) h << "  P = (MSP)0;\n";
        h << "  O = (MSV)0; prim_ = 0u; hasv_ = false; hasp_ = false;\n";
        h << "  prog(gid_, gi_);\n  GroupMemoryBarrierWithGroupSync();\n";
        if (uses_clipcull) h << "  " << clipcull_copy() << "\n";
        h << "  uint nv = min(ms_counts_ & 0xfffu, " << ms_max_verts << "u), np = min(ms_counts_ >> 12, " << ms_max_prims << "u);\n";
        h << "  SetMeshOutputCounts(nv, np);\n";
        h << "  if (gi_ < nv) verts[gi_] = O;   // unexported slots stay zero\n";
        if (has_prims()) h << "  if (gi_ < np) prims[gi_] = P;\n";
        h << "  if (gi_ < np) tris[gi_] = (hasp_ && (prim_ >> 31) == 0u) ? uint3(prim_ & 0x1ffu, (prim_ >> 10) & 0x1ffu, (prim_ >> 20) & 0x1ffu) : uint3(0u, 0u, 0u);\n";
        h << "}\n";
    }
    return h.str();
}

}  // namespace

// ---- Geometry shaders that copy their input vertices -------------------------------------------
// Console geometry shaders read the vertex shader's outputs from LDS (ES-GS items at the per-vertex
// offsets in v0..v2) and write emitted vertices back to LDS before s_sendmsg(GS, EMIT). Many (e.g.
// passes that pick a render-target slice) only copy fields. The program is run for one lane with
// every input dword tagged; when each written output dword is an unmodified input dword (or a
// constant), the shader becomes a PC geometry shader that appends those copies.
namespace {
struct GsVal { uint32_t v = 0; int tag = -1; };   // tag: input (vertex << 12 | dword), -1 none
constexpr uint32_t kGsIn = 0x4000, kGsVtx = 0x1000;  // tagged input items: byte 0x4000 + vertex * 0x1000

bool translate_gs_copy(const Container& c, TranslateResult& r) {
    auto fail = [&](const std::string& e) { r.error = "GS: " + e; return false; };
    std::vector<SigElem> isg, osg;
    if (auto* p = c.part("ISG1")) isg = parse_signature(*p);
    if (auto* p = c.part("OSG1")) osg = parse_signature(*p);
    if (isg.empty() || osg.empty()) return fail("no signatures");
    unsigned in_prim = 3, out_topo = 5;   // D3D_PRIMITIVE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
    if (auto* p = c.part("PSV0"); p && p->size() >= 12) {
        memcpy(&in_prim, p->data() + 4, 4);
        memcpy(&out_topo, p->data() + 8, 4);
    }
    unsigned nverts = in_prim == 1 ? 1 : in_prim == 2 ? 2 : in_prim == 3 ? 3 : in_prim == 6 ? 4 : in_prim == 7 ? 6 : 0;
    if (!nverts) return fail(fmt("input primitive %u not supported", in_prim));
    if (!c.has_native || c.code_end <= c.body_begin) return fail("no machine code");
    auto ins = decode_all(reinterpret_cast<const uint32_t*>(c.precompiled.data() + c.body_begin), (c.code_end - c.body_begin) / 4);

    GsVal S[128], V[256];
    uint32_t vcc = 0, exec = 1, m0 = 0;
    bool scc = false;
    for (unsigned k = 0; k < 3; k++)   // v0..v2: ES-GS offsets (dwords) of vertices 2k and 2k+1
        V[k].v = (kGsIn / 4 + 2 * k * (kGsVtx / 4)) | ((kGsIn / 4 + (2 * k + 1) * (kGsVtx / 4)) << 16);
    std::map<uint32_t, GsVal> lds_out;   // written LDS dwords
    struct Out { unsigned vtx; uint32_t addr; GsVal val; };
    std::vector<Out> outs;
    std::vector<int> events;             // per emitted vertex: 0 = emit, 1 = emit then cut, 2 = cut only
    unsigned emitted = 0;
    auto lit = [](const Instr& in) { return in.literal; };
    auto rd = [&](const Instr& in, uint16_t e) -> GsVal {
        if (e < 106) return S[e];
        if (e == 106) return {vcc, -1};
        if (e == 124) return {m0, -1};
        if (e == 126) return {exec, -1};
        if (e == 127 || e == 107) return {0, -1};
        if (e >= 128 && e <= 192) return {uint32_t(e - 128), -1};
        if (e >= 193 && e <= 208) return {uint32_t(-(int)(e - 192)), -1};
        if (e == 255) return {lit(in), -1};
        if (e >= 256) return V[e - 256];
        static const uint32_t fc[8] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000, 0x40000000, 0xc0000000, 0x40800000, 0xc0800000};
        if (e >= 240 && e <= 247) return {fc[e - 240], -1};
        return {0, -1};
    };
    auto ws = [&](uint16_t d, uint32_t v) {
        if (d < 106) S[d] = {v, -1};
        else if (d == 106) vcc = v;
        else if (d == 124) m0 = v;
        else if (d == 126) exec = v;
    };
    auto sel = [](uint32_t v, uint8_t s) {
        if (s <= 3) return (v >> (8 * s)) & 0xff;
        if (s == 4) return v & 0xffff;
        if (s == 5) return v >> 16;
        return v;
    };
    auto lds_read = [&](uint32_t a) -> GsVal {
        if (auto it = lds_out.find(a); it != lds_out.end()) return it->second;
        if (a >= kGsIn && a < kGsIn + nverts * kGsVtx) {
            uint32_t k = (a - kGsIn) / kGsVtx, dw = ((a - kGsIn) % kGsVtx) / 4;
            return {0, int(k << 12 | dw)};
        }
        return {0, -1};
    };
    auto lds_write = [&](uint32_t a, GsVal v) {
        lds_out[a] = v;
        outs.push_back({emitted, a, v});
    };
    for (auto& in : ins) {
        std::string n = in.name ? in.name : "";
        if (n.empty()) return fail(fmt("unknown instruction at 0x%x", in.offset));
        if (n == "s_endpgm") break;
        if (n == "s_inst_prefetch" || n == "s_waitcnt" || n == "s_nop" || n == "s_waitcnt_vscnt" || n == "s_waitcnt_vmcnt" ||
            n == "s_waitcnt_lgkmcnt" || n == "s_waitcnt_expcnt" || n == "s_dcache_inv")
            continue;
        if (n == "s_sendmsg") {
            unsigned id = in.simm16 & 0xf, op = (in.simm16 >> 4) & 3, stream = (in.simm16 >> 8) & 3;
            if (id == 3) continue;   // GS_DONE
            if (id != 2 || stream) return fail(fmt("message 0x%x not supported", in.simm16));
            if (op == 2 || op == 3) { events.push_back(op == 3 ? 1 : 0); emitted++; }
            else if (op == 1) events.push_back(2);
            continue;
        }
        if (starts(n, "s_load_") || starts(n, "s_buffer_load_")) {   // GS ring/state constants: zero
            unsigned cnt = ends(n, "x16") ? 16 : ends(n, "x8") ? 8 : ends(n, "x4") ? 4 : ends(n, "x2") ? 2 : 1;
            for (unsigned k = 0; k < cnt; k++) ws(in.sdata + k, 0);
            continue;
        }
        if (in.fmt == Fmt::SOP1 || in.fmt == Fmt::SOP2 || in.fmt == Fmt::SOPC || in.fmt == Fmt::SOPK) {
            uint32_t a = rd(in, in.src0).v, b = rd(in, in.src1).v;
            if (in.fmt == Fmt::SOPK) a = uint32_t(in.simm16);
            uint32_t d;
            if (n == "s_mov_b32" || n == "s_movk_i32") d = a;
            else if (n == "s_mov_b64") { ws(in.dst, a); ws(in.dst + 1, in.src0 >= 128 && in.src0 <= 208 ? (in.src0 >= 193 ? ~0u : 0u) : rd(in, in.src0 + 1).v); continue; }
            else if (n == "s_and_b32") d = a & b;
            else if (n == "s_or_b32") d = a | b;
            else if (n == "s_xor_b32") d = a ^ b;
            else if (n == "s_lshl_b32") d = a << (b & 31);
            else if (n == "s_lshr_b32") d = a >> (b & 31);
            else if (n == "s_add_i32" || n == "s_add_u32") d = a + b;
            else if (n == "s_sub_i32" || n == "s_sub_u32") d = a - b;
            else if (n == "s_mul_i32") d = a * b;
            else if (n == "s_bfe_u32" || n == "s_bfe_i32") {
                unsigned off = b & 31, w = (b >> 16) & 0x7f;
                d = w ? (a >> off) & (w >= 32 ? ~0u : ((1u << w) - 1)) : 0;
                if (n == "s_bfe_i32" && w && w < 32 && (d >> (w - 1)) & 1) d |= ~((1u << w) - 1);
            }
            else if (n == "s_bfm_b32") d = ((1u << (a & 31)) - 1) << (b & 31);
            else if (n == "s_bfm_b64") { uint64_t m = ((a & 63) ? (~0ull >> (64 - (a & 63))) : 0) << (b & 63); ws(in.dst, uint32_t(m)); ws(in.dst + 1, uint32_t(m >> 32)); continue; }
            else if (n == "s_cmp_eq_u32" || n == "s_cmp_eq_i32") { scc = a == b; continue; }
            else if (n == "s_cmp_lg_u32" || n == "s_cmp_lg_i32") { scc = a != b; continue; }
            else if (n == "s_cselect_b32") d = scc ? a : b;
            else if (n == "s_cselect_b64") { uint32_t hi_a = in.src0 >= 128 ? (in.src0 >= 193 && in.src0 <= 208 ? ~0u : 0u) : rd(in, in.src0 + 1).v;
                                             uint32_t hi_b = in.src1 >= 128 ? (in.src1 >= 193 && in.src1 <= 208 ? ~0u : 0u) : rd(in, in.src1 + 1).v;
                                             ws(in.dst, scc ? a : b); ws(in.dst + 1, scc ? hi_a : hi_b); continue; }
            else return fail(n + " not supported");
            ws(in.dst, d);
            continue;
        }
        if (starts(n, "v_")) {
            if (in.dpp) return fail(n + " with DPP not supported");
            GsVal a = rd(in, in.src0), b = rd(in, in.src1), c2 = rd(in, in.src2);
            if (in.sdwa) {
                a = in.sdwa_f.s0 ? S[in.sdwa_f.src0] : V[in.sdwa_f.src0];
                a.v = sel(a.v, in.sdwa_f.src0_sel);
                b.v = sel(b.v, in.sdwa_f.src1_sel);
                if (in.sdwa_f.src0_sel != 6) a.tag = -1;
            }
            GsVal d{0, -1};
            if (n == "v_mov_b32") d = a;
            else if (n == "v_and_b32") d.v = a.v & b.v;
            else if (n == "v_or_b32") d.v = a.v | b.v;
            else if (n == "v_xor_b32") d.v = a.v ^ b.v;
            else if (n == "v_lshlrev_b32") d.v = b.v << (a.v & 31);
            else if (n == "v_lshrrev_b32") d.v = b.v >> (a.v & 31);
            else if (n == "v_add_nc_u32" || n == "v_add_u32") d.v = a.v + b.v;
            else if (n == "v_sub_nc_u32") d.v = a.v - b.v;
            else if (n == "v_mul_u32_u24") d.v = (a.v & 0xffffff) * (b.v & 0xffffff);
            else if (n == "v_mad_u32_u24") d.v = (a.v & 0xffffff) * (b.v & 0xffffff) + c2.v;
            else if (n == "v_lshl_add_u32") d.v = (a.v << (b.v & 31)) + c2.v;
            else if (n == "v_lshl_or_b32") d.v = (a.v << (b.v & 31)) | c2.v;
            else if (n == "v_and_or_b32") d.v = (a.v & b.v) | c2.v;
            else if (n == "v_bfe_u32") d.v = (c2.v & 31) ? (a.v >> (b.v & 31)) & ((1u << (c2.v & 31)) - 1) : 0;
            else if (n == "v_mbcnt_lo_u32_b32" || n == "v_mbcnt_hi_u32_b32") d.v = b.v;   // lane 0
            else return fail(n + " not supported");
            V[in.dst] = d;
            continue;
        }
        if (in.fmt == Fmt::DS) {
            uint32_t base = V[in.vaddr].v, o0 = uint32_t(in.offset_imm), o1 = uint32_t(in.gds_offset1);
            if (n == "ds_read_b32") { V[in.dst] = lds_read(base + (o0 | o1 << 8)); continue; }
            if (n == "ds_read_b64") { for (unsigned k = 0; k < 2; k++) V[in.dst + k] = lds_read(base + (o0 | o1 << 8) + 4 * k); continue; }
            if (n == "ds_read2_b32") { V[in.dst] = lds_read(base + o0 * 4); V[in.dst + 1] = lds_read(base + o1 * 4); continue; }
            if (n == "ds_write_b32") { lds_write(base + (o0 | o1 << 8), V[in.data0]); continue; }
            if (n == "ds_write_b64") { for (unsigned k = 0; k < 2; k++) lds_write(base + (o0 | o1 << 8) + 4 * k, V[in.data0 + k]); continue; }
            if (n == "ds_write2_b32") { lds_write(base + o0 * 4, V[in.data0]); lds_write(base + o1 * 4, V[in.data1]); continue; }
            return fail(n + " not supported");
        }
        return fail(fmt("%s at 0x%x not supported (only copying geometry shaders are translated)", n.c_str(), in.offset));
    }
    if (!emitted) return fail("no vertices emitted");
    // Output layout: vertex k's dword d at base + (k * stride + d) * 4.
    unsigned regs = 0;
    for (auto& e : osg) regs = std::max(regs, e.reg + 1);
    const unsigned stride = regs * 4;
    uint32_t base = UINT32_MAX;
    for (auto& o : outs) base = std::min(base, o.addr);
    std::map<std::pair<unsigned, unsigned>, GsVal> vtx_dw;   // (vertex, dword) -> value (last write)
    for (auto& o : outs) {
        unsigned rel = (o.addr - base) / 4, k = rel / stride, d = rel % stride;
        if (k != o.vtx) return fail(fmt("output write for vertex %u lands in slot %u", o.vtx, k));
        vtx_dw[{k, d}] = o.val;
    }
    auto sorted = [](std::vector<SigElem> v) {
        std::sort(v.begin(), v.end(), [](const SigElem& a, const SigElem& b) { return a.reg != b.reg ? a.reg < b.reg : (a.mask & -a.mask) < (b.mask & -b.mask); });
        return v;
    };
    auto sem = [](const SigElem& e) { return e.index ? fmt("%s%u", e.name.c_str(), e.index) : e.name; };
    auto field = [](const SigElem& e, unsigned comp) -> std::string {
        static const char* xyzw = "xyzw";
        unsigned first = __builtin_ctz(e.mask), w = __builtin_popcount(e.mask);
        std::string f = e.sysval == 1 ? "pos_" : e.sysval == 4 ? "rtai_" : e.sysval == 5 ? "vpi_" : fmt("o%u_%u", e.reg, first);
        if (e.sysval == 1) return f + "." + xyzw[comp];
        return w > 1 ? f + "." + xyzw[comp - first] : f;
    };
    auto decl = [&](const std::vector<SigElem>& sig, const char* name) {
        std::string o = fmt("struct %s {\n", name);
        bool pos = false;
        for (auto& e : sorted(sig)) {
            if (e.sysval == 1) { o += "  float4 pos_ : SV_Position;\n"; pos = true; continue; }
            if (e.sysval == 4) { o += "  uint rtai_ : SV_RenderTargetArrayIndex;\n"; continue; }
            if (e.sysval == 5) { o += "  uint vpi_ : SV_ViewportArrayIndex;\n"; continue; }
            if (e.sysval != 0) continue;
            unsigned w = unsigned(__builtin_popcount(e.mask));
            const char* b = e.base_type();
            o += "  " + (w > 1 ? fmt("%s%u", b, w) : std::string(b)) + fmt(" o%u_%u : ", e.reg, __builtin_ctz(e.mask)) + sem(e) + ";\n";
        }
        if (!pos) o += "  float4 pos_ : SV_Position;\n";
        return o + "};\n";
    };
    auto elem_at = [](const std::vector<SigElem>& sig, unsigned reg, unsigned comp) -> const SigElem* {
        for (auto& e : sig) if (e.reg == reg && (e.mask >> comp & 1) && (e.sysval <= 1 || e.sysval == 4 || e.sysval == 5)) return &e;
        return nullptr;
    };
    std::ostringstream h;
    h << "// Translated by OpenScarlett gpucode from console GPU machine code (copying geometry shader).\n";
    h << decl(isg, "GSIn") << decl(osg, "GSOut");
    static const char* prim[] = {"", "point", "line", "triangle", "", "", "lineadj", "triangleadj"};
    const char* stream = out_topo == 1 ? "PointStream" : out_topo == 3 ? "LineStream" : "TriangleStream";
    h << "[maxvertexcount(" << emitted << ")]\nvoid main(" << prim[in_prim] << " GSIn I[" << nverts << "], inout " << stream << "<GSOut> S) {\n  GSOut O;\n";
    unsigned k = 0;
    for (int ev : events) {
        if (ev == 2) { h << "  S.RestartStrip();\n"; continue; }
        h << "  O = (GSOut)0;\n";
        for (auto& e : osg) {
            for (unsigned comp = 0; comp < 4; comp++) {
                if (!(e.mask >> comp & 1)) continue;
                if (e.sysval > 1 && e.sysval != 4 && e.sysval != 5) continue;
                auto it = vtx_dw.find({k, e.reg * 4 + comp});
                if (it == vtx_dw.end()) continue;   // not written: stays zero
                std::string bits;
                if (it->second.tag >= 0) {
                    unsigned sv = unsigned(it->second.tag) >> 12, sd = unsigned(it->second.tag) & 0xfff;
                    const SigElem* ie = elem_at(isg, sd / 4, sd % 4);
                    if (!ie || sv >= nverts) return fail(fmt("vertex %u copies input dword %u of vertex %u, which is not in the input signature", k, sd, sv));
                    bits = fmt("asuint(I[%u].%s)", sv, field(*ie, sd % 4).c_str());
                } else {
                    bits = fmt("%uu", it->second.v);
                }
                const char* cast = e.sysval == 4 || e.sysval == 5 || e.is_uint() ? "" : e.is_sint() ? "asint" : "asfloat";
                h << "  O." << field(e, comp) << " = " << cast << "(" << bits << ");\n";
            }
        }
        h << "  S.Append(O);\n";
        if (ev == 1) h << "  S.RestartStrip();\n";
        k++;
    }
    h << "}\n";
    r.hlsl = h.str();
    r.profile = "gs_6_0";
    r.wave_size = 32;
    r.ok = true;
    return true;
}
}  // namespace

// ---- cache key salts ---------------------------------------------------------------------------
uint64_t translation_salt(const Container& c) {
    const std::vector<uint32_t> code = c.code();
    bool barrier = false, backward = false, global_write = false, computed = false, gather_level = false;
    for (size_t i = 0; i < code.size();) {
        Instr in;
        if (!decode(code.data(), code.size(), i, in)) break;
        i += in.len ? in.len : 1;
        if (!in.name) continue;
        if (!strcmp(in.name, "s_barrier")) barrier = true;
        if ((starts(in.name, "s_cbranch") || !strcmp(in.name, "s_branch")) && in.simm16 < 0) backward = true;
        if (!strcmp(in.name, "s_setpc_b64") || !strcmp(in.name, "s_swappc_b64")) computed = true;
        if (starts(in.name, "global_store_") || starts(in.name, "global_atomic_")) global_write = true;
        if (starts(in.name, "image_gather4_l") && !starts(in.name, "image_gather4_lz")) gather_level = true;
    }
    uint64_t h = 0;
    auto add = [&](const char* tag) { for (const char* p = tag; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull + 0x9e3779b97f4a7c15ull; };
    // 2026-11: the step limit of programs with barriers and loops went from 2^22 to 2^16, with the
    // small limit past the last barrier.
    // 2026-11: with loops the threads of a group take turns between barriers; without, every
    // thread makes each barrier call.
    // 2026-11: in code with loops a block only fallen into continues the case before it.
    if (backward || computed) add("inline-fall-1");
    if (barrier && backward) add("barrier-coop-1");
    if (barrier && !backward) add("barrier-all-1");
    // 2026-11: global stores take their data from the data field (they read v0); global atomics.
    if (global_write) add("global-write-2");
    // 2026-11: a gather at a named level reads that level (it read level 0).
    if (gather_level) add("gather-level-1");
    // 2026-11: 16-bit vertex and mesh outputs are unpacked into the registers their records name
    // (they went out as 16-bit patterns, and a packed pair's later registers into the wrong ones).
    Meta m;
    if ((c.kind == ProgramKind::VS || c.kind == ProgramKind::MS) && parse_meta(c, m)) {
        bool half = false;
        for (auto& o : m.vs_outputs) half |= o.fp16;
        if (half) add("fp16-out-1");
    }
    return h;
}

TranslateResult translate(const Container& c, const RootSig& rs, const TranslateOptions& opt) {
    TranslateResult r;
    if (c.kind == ProgramKind::GS) {
        translate_gs_copy(c, r);
        return r;
    }
    Translator t(c, rs, opt);
    if (c.kind != ProgramKind::CS && c.kind != ProgramKind::PS && c.kind != ProgramKind::VS && c.kind != ProgramKind::MS) {
        r.error = std::string("stage ") + kind_name(c.kind) + " not supported yet";
        return r;
    }
    if (!t.load()) {
        r.error = t.err;
        return r;
    }
    t.build_cfg();
    t.analyse();
    t.link_arrays();
    if (!t.err.empty()) {
        r.error = t.err;
        return r;
    }
    r.hlsl = t.generate();
    r.wave_size = t.W;
    if (opt.raw_stats) {   // counters in the global-memory helpers (see TranslateOptions::raw_stats)
        auto rep = [&](const std::string& a, const std::string& b) {
            size_t at = r.hlsl.find(a);
            if (at != std::string::npos) r.hlsl.replace(at, a.size(), b);
        };
        rep("StructuredBuffer<uint4> GMT : register(t0, space901);\n",
            "RWByteAddressBuffer RTDBG : register(u0, space932);\nstatic uint DBGB = 0u;\n"
            "void dbg_add(uint slot) { uint o_; RTDBG.InterlockedAdd(DBGB + slot * 4u, 1u, o_); }\n"
            "void dbg_set2(uint slot, uint a, uint b) { RTDBG.Store2(DBGB + slot * 4u, uint2(a, b)); }\n"
            "StructuredBuffer<uint4> GMT : register(t0, space901);\n");
        rep("uint gm_load(uint lo, uint hi) {\n  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n  if (!fx) return 0u;\n",
            "uint gm_load(uint lo, uint hi) {\n  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n  dbg_add(21u);\n"
            "  if (!fx) { dbg_add(12u); dbg_set2(13u, lo, hi); return 0u; }\n");
        rep("void gm_store(uint lo, uint hi, uint v) {\n  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n",
            "void gm_store(uint lo, uint hi, uint v) {\n  uint fy, fz, fl; bool fx = gm_find(lo, hi, fy, fz, fl);\n  dbg_add(22u);\n"
            "  if (!fx || !(fy & 0x80000000u)) { dbg_add(15u); dbg_set2(16u, lo, hi); }\n");
    }
    // Conservative depth outputs need the position input interpolated noperspective centroid (DXIL
    // validation; the console hardware has no such rule).
    if (c.kind == ProgramKind::PS && (r.hlsl.find("SV_DepthLessEqual") != std::string::npos || r.hlsl.find("SV_DepthGreaterEqual") != std::string::npos)) {
        const std::string a = "  float4 pos_ : SV_Position;\n";
        size_t at = r.hlsl.find(a);
        if (at != std::string::npos) r.hlsl.replace(at, a.size(), "  noperspective centroid float4 pos_ : SV_Position;\n");
    }
    if (!t.err.empty()) {
        r.error = t.err;
        r.hlsl.clear();
        return r;
    }
    r.ok = true;
    if (c.kind == ProgramKind::CS) r.profile = t.uses_quad || (t.dual && opt.wave_size_attr) ? "cs_6_6" : t.uses_dot4 && opt.dot4_intrinsic ? "cs_6_4" : "cs_6_0";
    else if (c.kind == ProgramKind::PS) r.profile = "ps_6_0";
    else if (c.kind == ProgramKind::MS) r.profile = "ms_6_5";
    else r.profile = "vs_6_0";
    return r;
}

}  // namespace gpucode
