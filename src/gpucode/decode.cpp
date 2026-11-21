// SPDX-License-Identifier: AGPL-3.0-or-later
// GFX10 instruction decoder. Encoding field positions follow AMD's public RDNA ISA documentation.
// Accepts the Xbox compiler's encodings that stock LLVM rejects (SGPR tuples reaching VCC, VOPC
// with a DPP16 operand): they are valid hardware encodings.
#include "isa.h"

#include <cstdio>
#include <cstring>

namespace gpucode {
namespace {

struct Entry { Fmt f; uint16_t op; const char* name; };
const Entry kTable[] = {
#define XSL_ISA_OP(f, op, name) {Fmt::f, op, name},
#include "isa_tables.inc"
#undef XSL_ISA_OP
};

// Dense lookup: [fmt][op], op < 1024.
struct Names {
    const char* t[size_t(Fmt::Count)][1024] = {};
    Names() {
        for (auto& e : kTable) t[size_t(e.f)][e.op] = e.name;
    }
};
const Names& names() {
    static Names n;
    return n;
}

inline uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t sext(uint32_t v, int n) { return int32_t(v << (32 - n)) >> (32 - n); }

bool name_has(const char* n, const char* s) { return n && strstr(n, s); }

}  // namespace

const char* fmt_name(Fmt f) {
    static const char* n[] = {"SOP2", "SOPK", "SOP1", "SOPC", "SOPP", "SMEM", "VOP2", "VOP1", "VOPC", "VOP3",
                              "VOP3P", "VINTRP", "DS", "MUBUF", "MTBUF", "MIMG", "EXP", "FLAT", "SCRATCH",
                              "GLOBAL", "Invalid"};
    return f < Fmt::Count ? n[size_t(f)] : "?";
}

const char* op_name(Fmt f, unsigned op) {
    if (f >= Fmt::Count || op >= 1024) return nullptr;
    return names().t[size_t(f)][op];
}

bool decode(const uint32_t* code, size_t n, size_t i, Instr& in) {
    if (i >= n) return false;
    in = Instr{};
    in.offset = uint32_t(i * 4);
    const uint32_t x = code[i];
    auto need = [&](unsigned dw) { return i + dw <= n; };
    unsigned len = 1;
    auto lit_if = [&](bool cond) {
        if (cond) {
            in.has_literal = true;
            len++;
        }
    };

    if ((x >> 31) == 0) {   // VOP2 / VOP1 / VOPC
        const uint32_t op6 = bits(x, 25, 6);
        in.src0 = uint16_t(bits(x, 0, 9));
        if (op6 == 0x3f) {
            in.fmt = Fmt::VOP1;
            in.op = uint16_t(bits(x, 9, 8));
            in.dst = uint16_t(bits(x, 17, 8));
        } else if (op6 == 0x3e) {
            in.fmt = Fmt::VOPC;
            in.op = uint16_t(bits(x, 17, 8));
            in.src1 = uint16_t(256 + bits(x, 9, 8));
        } else {
            in.fmt = Fmt::VOP2;
            in.op = uint16_t(op6);
            in.src1 = uint16_t(256 + bits(x, 9, 8));
            in.dst = uint16_t(bits(x, 17, 8));
        }
        in.name = op_name(in.fmt, in.op);
        if (in.src0 == src::kSdwa) {
            if (!need(2)) return false;
            const uint32_t y = code[i + 1];
            in.sdwa = true;
            auto& s = in.sdwa_f;
            s.src0 = uint8_t(bits(y, 0, 8));
            s.s0 = bits(y, 23, 1);
            s.s1 = bits(y, 31, 1);
            s.src0_sel = uint8_t(bits(y, 16, 3));
            s.sext0 = bits(y, 19, 1);
            s.neg0 = bits(y, 20, 1);
            s.abs0 = bits(y, 21, 1);
            s.src1_sel = uint8_t(bits(y, 24, 3));
            s.sext1 = bits(y, 27, 1);
            s.neg1 = bits(y, 28, 1);
            s.abs1 = bits(y, 29, 1);
            if (in.fmt == Fmt::VOPC) {
                s.sdst = uint8_t(bits(y, 8, 7));
                s.sd = bits(y, 15, 1);
            } else {
                s.dst_sel = uint8_t(bits(y, 8, 3));
                s.dst_unused = uint8_t(bits(y, 11, 2));
                s.clamp = bits(y, 13, 1);
                s.omod = uint8_t(bits(y, 14, 2));
            }
            in.src0 = uint16_t(s.s0 ? s.src0 : 256 + s.src0);
            if (in.fmt != Fmt::VOP1 && s.s1) in.src1 = uint16_t(in.src1 - 256);
            len = 2;
        } else if (in.src0 == src::kDpp16 || in.src0 == src::kDpp8 || in.src0 == src::kDpp8Fi) {
            if (!need(2)) return false;
            const uint32_t y = code[i + 1];
            in.dpp = true;
            auto& d = in.dpp_f;
            d.src0 = uint8_t(bits(y, 0, 8));
            if (in.src0 == src::kDpp16) {
                d.ctrl = uint16_t(bits(y, 8, 9));
                d.fi = bits(y, 18, 1);
                d.bound_ctrl = bits(y, 19, 1);
                d.neg0 = bits(y, 20, 1);
                d.abs0 = bits(y, 21, 1);
                d.neg1 = bits(y, 22, 1);
                d.abs1 = bits(y, 23, 1);
                d.bank_mask = uint8_t(bits(y, 24, 4));
                d.row_mask = uint8_t(bits(y, 28, 4));
            } else {
                d.dpp8 = true;
                d.fi = in.src0 == src::kDpp8Fi;
                for (int k = 0; k < 8; k++) d.sel[k] = uint8_t(bits(y, 8 + 3 * k, 3));
            }
            in.src0 = uint16_t(256 + d.src0);
            len = 2;
        } else {
            lit_if(in.src0 == src::kLiteral);
        }
        // v_madmk/v_madak/v_fmamk/v_fmaak (f32 and f16) always carry a literal constant.
        if (in.fmt == Fmt::VOP2 && !in.has_literal && (name_has(in.name, "mk_f") || name_has(in.name, "ak_f"))) lit_if(true);
    } else if ((x >> 23) == 0x17d) {   // SOP1
        in.fmt = Fmt::SOP1;
        in.op = uint16_t(bits(x, 8, 8));
        in.dst = uint16_t(bits(x, 16, 7));
        in.src0 = uint16_t(bits(x, 0, 8));
        lit_if(in.src0 == src::kLiteral);
    } else if ((x >> 23) == 0x17e) {   // SOPC
        in.fmt = Fmt::SOPC;
        in.op = uint16_t(bits(x, 16, 7));
        in.src0 = uint16_t(bits(x, 0, 8));
        in.src1 = uint16_t(bits(x, 8, 8));
        lit_if(in.src0 == src::kLiteral || in.src1 == src::kLiteral);
    } else if ((x >> 23) == 0x17f) {   // SOPP
        in.fmt = Fmt::SOPP;
        in.op = uint16_t(bits(x, 16, 7));
        in.simm16 = sext(bits(x, 0, 16), 16);
    } else if ((x >> 28) == 0xb) {     // SOPK
        in.fmt = Fmt::SOPK;
        in.op = uint16_t(bits(x, 23, 5));
        in.dst = uint16_t(bits(x, 16, 7));
        in.simm16 = sext(bits(x, 0, 16), 16);
        in.name = op_name(in.fmt, in.op);
        lit_if(name_has(in.name, "s_setreg_imm32"));
    } else if ((x >> 30) == 2) {       // SOP2
        in.fmt = Fmt::SOP2;
        in.op = uint16_t(bits(x, 23, 7));
        in.dst = uint16_t(bits(x, 16, 7));
        in.src0 = uint16_t(bits(x, 0, 8));
        in.src1 = uint16_t(bits(x, 8, 8));
        lit_if(in.src0 == src::kLiteral || in.src1 == src::kLiteral);
    } else {
        const uint32_t top6 = x >> 26;
        if (!need(2) && top6 != 0x32) return false;
        const uint32_t y = top6 != 0x32 ? code[i + 1] : 0;
        switch (top6) {
        case 0x35:   // VOP3
            in.fmt = Fmt::VOP3;
            in.op = uint16_t(bits(x, 16, 10));
            in.dst = uint16_t(bits(x, 0, 8));
            in.abs = uint8_t(bits(x, 8, 3));
            in.sdst = uint16_t(bits(x, 8, 7));
            in.opsel = uint8_t(bits(x, 11, 4));
            in.clamp = bits(x, 15, 1);
            in.src0 = uint16_t(bits(y, 0, 9));
            in.src1 = uint16_t(bits(y, 9, 9));
            in.src2 = uint16_t(bits(y, 18, 9));
            in.omod = uint8_t(bits(y, 27, 2));
            in.neg = uint8_t(bits(y, 29, 3));
            len = 2;
            lit_if(in.src0 == src::kLiteral || in.src1 == src::kLiteral || in.src2 == src::kLiteral);
            break;
        case 0x33:   // VOP3P
            in.fmt = Fmt::VOP3P;
            in.op = uint16_t(bits(x, 16, 7));
            in.dst = uint16_t(bits(x, 0, 8));
            in.neg_hi = uint8_t(bits(x, 8, 3));
            in.opsel = uint8_t(bits(x, 11, 3));
            in.opsel_hi = uint8_t(bits(x, 14, 1) << 2 | bits(y, 27, 2));
            in.clamp = bits(x, 15, 1);
            in.src0 = uint16_t(bits(y, 0, 9));
            in.src1 = uint16_t(bits(y, 9, 9));
            in.src2 = uint16_t(bits(y, 18, 9));
            in.neg = uint8_t(bits(y, 29, 3));
            len = 2;
            lit_if(in.src0 == src::kLiteral || in.src1 == src::kLiteral || in.src2 == src::kLiteral);
            break;
        case 0x32:   // VINTRP
            in.fmt = Fmt::VINTRP;
            in.op = uint16_t(bits(x, 16, 2));
            in.src0 = uint16_t(256 + bits(x, 0, 8));
            in.attr_chan = uint8_t(bits(x, 8, 2));
            in.attr = uint8_t(bits(x, 10, 6));
            in.dst = uint16_t(bits(x, 18, 8));
            break;
        case 0x36:   // DS
            in.fmt = Fmt::DS;
            in.op = uint16_t(bits(x, 18, 8));
            in.offset_imm = int32_t(bits(x, 0, 8));
            in.gds_offset1 = uint16_t(bits(x, 8, 8));
            in.gds = bits(x, 17, 1);
            in.vaddr = uint16_t(bits(y, 0, 8));
            in.data0 = uint16_t(bits(y, 8, 8));
            in.data1 = uint16_t(bits(y, 16, 8));
            in.dst = uint16_t(bits(y, 24, 8));
            len = 2;
            break;
        case 0x38:   // MUBUF
        case 0x3a:   // MTBUF
            in.fmt = top6 == 0x38 ? Fmt::MUBUF : Fmt::MTBUF;
            in.offset_imm = int32_t(bits(x, 0, 12));
            in.offen = bits(x, 12, 1);
            in.idxen = bits(x, 13, 1);
            in.glc = bits(x, 14, 1);
            in.dlc = bits(x, 15, 1);
            if (in.fmt == Fmt::MUBUF) {
                in.lds = bits(x, 16, 1);
                in.op = uint16_t(bits(x, 18, 8));
            } else {
                in.op = uint16_t(bits(x, 16, 3) | bits(y, 21, 1) << 3);
                in.format = uint8_t(bits(x, 19, 7));
            }
            in.vaddr = uint16_t(bits(y, 0, 8));
            in.vdata = uint16_t(bits(y, 8, 8));
            in.srsrc = uint16_t(bits(y, 16, 5) * 4);
            in.slc = bits(y, 22, 1);
            in.tfe = bits(y, 23, 1);
            in.soffset = uint16_t(bits(y, 24, 8));
            len = 2;
            lit_if(in.soffset == src::kLiteral);
            break;
        case 0x3c: {   // MIMG
            in.fmt = Fmt::MIMG;
            in.op = uint16_t(bits(x, 18, 7) | bits(x, 0, 1) << 7);
            in.nsa = uint8_t(bits(x, 1, 2));
            in.dim = uint8_t(bits(x, 3, 3));
            in.dlc = bits(x, 7, 1);
            in.dmask = uint8_t(bits(x, 8, 4));
            in.unorm = bits(x, 12, 1);
            in.glc = bits(x, 13, 1);
            in.r128 = bits(x, 15, 1);
            in.tfe = bits(x, 16, 1);
            in.lwe = bits(x, 17, 1);
            in.slc = bits(x, 25, 1);
            in.vaddr = uint16_t(bits(y, 0, 8));
            in.vdata = uint16_t(bits(y, 8, 8));
            in.srsrc = uint16_t(bits(y, 16, 5) * 4);
            in.ssamp = uint16_t(bits(y, 21, 5) * 4);
            in.a16 = bits(y, 30, 1);
            in.d16 = bits(y, 31, 1);
            len = 2 + in.nsa;
            if (!need(len)) return false;
            for (unsigned k = 0; k < in.nsa; k++)
                for (unsigned b = 0; b < 4; b++) in.addr_extra[4 * k + b] = uint8_t(code[i + 2 + k] >> (8 * b));
            in.nsa = uint8_t(in.nsa * 4);   // number of extra address bytes (unused tail bytes are 0)
            break;
        }
        case 0x3d:   // SMEM
            in.fmt = Fmt::SMEM;
            in.op = uint16_t(bits(x, 18, 8));
            in.sbase = uint16_t(bits(x, 0, 6) * 2);
            in.sdata = uint16_t(bits(x, 6, 7));
            in.dlc = bits(x, 14, 1);
            in.glc = bits(x, 16, 1);
            in.offset_imm = sext(bits(y, 0, 21), 21);
            in.soffset = uint16_t(bits(y, 25, 7));
            len = 2;
            break;
        case 0x3e:   // EXP
            in.fmt = Fmt::EXP;
            in.exp_en = uint8_t(bits(x, 0, 4));
            in.exp_target = uint8_t(bits(x, 4, 6));
            in.exp_compr = bits(x, 10, 1);
            in.exp_done = bits(x, 11, 1);
            in.exp_vm = bits(x, 12, 1);
            for (int k = 0; k < 4; k++) in.exp_src[k] = uint8_t(y >> (8 * k));
            in.name = "exp";
            len = 2;
            break;
        case 0x37: {   // FLAT / SCRATCH / GLOBAL
            const uint32_t seg = bits(x, 14, 2);
            in.fmt = seg == 0 ? Fmt::FLAT : seg == 1 ? Fmt::SCRATCH : seg == 2 ? Fmt::GLOBAL : Fmt::Invalid;
            in.op = uint16_t(bits(x, 18, 7));
            in.offset_imm = in.fmt == Fmt::FLAT ? int32_t(bits(x, 0, 11)) : sext(bits(x, 0, 12), 12);
            in.dlc = bits(x, 12, 1);
            in.lds = bits(x, 13, 1);
            in.glc = bits(x, 16, 1);
            in.slc = bits(x, 17, 1);
            in.vaddr = uint16_t(bits(y, 0, 8));
            in.vdata = uint16_t(bits(y, 8, 8));
            in.saddr = uint16_t(bits(y, 16, 7));
            in.dst = uint16_t(bits(y, 24, 8));
            len = 2;
            break;
        }
        default:
            in.fmt = Fmt::Invalid;
            len = 1;
            break;
        }
    }
    if (!in.name) in.name = op_name(in.fmt, in.op);
    if (!need(len)) return false;
    in.len = uint8_t(len);
    for (unsigned k = 0; k < len && k < 4; k++) in.w[k] = code[i + k];
    if (in.has_literal) in.literal = code[i + len - 1];
    return true;
}

std::vector<Instr> decode_all(const uint32_t* code, size_t n) {
    std::vector<Instr> v;
    for (size_t i = 0; i < n;) {
        Instr in;
        if (!decode(code, n, i, in)) break;
        v.push_back(in);
        i += in.len;
    }
    return v;
}

std::string mnemonic_with_encoding(const Instr& in) {
    std::string n = in.name ? in.name : "<unknown>";
    switch (in.fmt) {
    case Fmt::VOP1: case Fmt::VOP2: case Fmt::VOPC:
        return n + (in.sdwa ? "_sdwa" : in.dpp ? "_dpp" : "_e32");
    case Fmt::VOP3:
        if (in.op < 0x140 || (in.op >= 0x180 && in.op < 0x200)) return n + "_e64";
        return n;
    default:
        return n;
    }
}

namespace {
std::string sreg(unsigned r, unsigned count = 1) {
    char b[32];
    auto one = [&](unsigned v) -> std::string {
        if (v < 106) return "s" + std::to_string(v);
        switch (v) {
        case src::kVccLo: return "vcc_lo";
        case src::kVccHi: return "vcc_hi";
        case src::kM0: return "m0";
        case src::kNull: return "null";
        case src::kExecLo: return "exec_lo";
        case src::kExecHi: return "exec_hi";
        }
        if (v >= src::kTtmp0 && v < src::kTtmp0 + 16) return "ttmp" + std::to_string(v - src::kTtmp0);
        return "s?" + std::to_string(v);
    };
    if (count == 1) return one(r);
    if (r == src::kVccLo && count == 2) return "vcc";
    if (r == src::kExecLo && count == 2) return "exec";
    if (r < 106) {
        snprintf(b, sizeof b, "s[%u:%u]", r, r + count - 1);
        return b;
    }
    return one(r) + "+" + std::to_string(count - 1);
}
std::string operand(const Instr& in, unsigned s, unsigned count = 1) {
    char b[48];
    if (s >= src::kVgpr0) {
        unsigned v = s - src::kVgpr0;
        if (count == 1) return "v" + std::to_string(v);
        snprintf(b, sizeof b, "v[%u:%u]", v, v + count - 1);
        return b;
    }
    if (s < 128 || (s >= src::kTtmp0 && s < 124 + 4)) return sreg(s, count);
    if (s >= src::kZero && s <= src::kIntPos64) return std::to_string(s - src::kZero);
    if (s >= src::kIntNeg1 && s <= src::kIntNeg16) return std::to_string(-int(s - src::kIntPos64));
    switch (s) {
    case src::kFloatHalf: return "0.5";
    case src::kFloatNegHalf: return "-0.5";
    case src::kFloatOne: return "1.0";
    case src::kFloatNegOne: return "-1.0";
    case src::kFloatTwo: return "2.0";
    case src::kFloatNegTwo: return "-2.0";
    case src::kFloatFour: return "4.0";
    case src::kFloatNegFour: return "-4.0";
    case src::kInvTwoPi: return "0.15915494";
    case src::kVccz: return "vccz";
    case src::kExecz: return "execz";
    case src::kScc: return "scc";
    case src::kLiteral:
        snprintf(b, sizeof b, "0x%x", in.literal);
        return b;
    case src::kShared_base: return "src_shared_base";
    case src::kPrivate_base: return "src_private_base";
    }
    return "src" + std::to_string(s);
}
}  // namespace

std::string disasm(const Instr& in) {
    std::string t = mnemonic_with_encoding(in);
    char b[96];
    switch (in.fmt) {
    case Fmt::SOP1: t += " " + sreg(in.dst) + ", " + operand(in, in.src0); break;
    case Fmt::SOP2: t += " " + sreg(in.dst) + ", " + operand(in, in.src0) + ", " + operand(in, in.src1); break;
    case Fmt::SOPC: t += " " + operand(in, in.src0) + ", " + operand(in, in.src1); break;
    case Fmt::SOPK: snprintf(b, sizeof b, " %s, 0x%x", sreg(in.dst).c_str(), in.simm16 & 0xffff); t += b; break;
    case Fmt::SOPP: snprintf(b, sizeof b, " %d", in.simm16); t += b; break;
    case Fmt::VOP1: t += " v" + std::to_string(in.dst) + ", " + operand(in, in.src0); break;
    case Fmt::VOP2: t += " v" + std::to_string(in.dst) + ", " + operand(in, in.src0) + ", " + operand(in, in.src1); break;
    case Fmt::VOPC: t += " " + operand(in, in.src0) + ", " + operand(in, in.src1); break;
    case Fmt::VOP3: case Fmt::VOP3P:
        t += " v" + std::to_string(in.dst) + ", " + operand(in, in.src0) + ", " + operand(in, in.src1) + ", " + operand(in, in.src2);
        break;
    case Fmt::SMEM:
        snprintf(b, sizeof b, " s%u, s[%u:%u], 0x%x", in.sdata, in.sbase, in.sbase + 1, in.offset_imm); t += b; break;
    case Fmt::MUBUF: case Fmt::MTBUF:
        snprintf(b, sizeof b, " v%u, v%u, s[%u:%u], %s offset:%d%s%s", in.vdata, in.vaddr, in.srsrc, in.srsrc + 3,
                 operand(in, in.soffset).c_str(), in.offset_imm, in.idxen ? " idxen" : "", in.offen ? " offen" : "");
        t += b;
        break;
    case Fmt::MIMG:
        snprintf(b, sizeof b, " v%u, v%u, s[%u:%u], s[%u:%u] dmask:0x%x dim:%u", in.vdata, in.vaddr, in.srsrc,
                 in.srsrc + 7, in.ssamp, in.ssamp + 3, in.dmask, in.dim);
        t += b;
        break;
    case Fmt::EXP:
        snprintf(b, sizeof b, " target%u v%u, v%u, v%u, v%u en:0x%x%s%s", in.exp_target, in.exp_src[0], in.exp_src[1],
                 in.exp_src[2], in.exp_src[3], in.exp_en, in.exp_compr ? " compr" : "", in.exp_done ? " done" : "");
        t = "exp" + std::string(b);
        break;
    default: break;
    }
    return t;
}

}  // namespace gpucode
