// SPDX-License-Identifier: AGPL-3.0-or-later
// GFX10 (Xbox Series X|S GPU) instruction decoding: formats, operands and the decoded instruction.
// Field layouts follow AMD's public RDNA instruction-set documentation; opcode names come from
// isa_tables.inc (tools/gen/gen_isa_tables.py). Host-independent: no Windows or console headers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gpucode {

enum class Fmt : uint8_t {
    SOP2, SOPK, SOP1, SOPC, SOPP, SMEM, VOP2, VOP1, VOPC, VOP3, VOP3P, VINTRP, DS, MUBUF, MTBUF, MIMG,
    EXP, FLAT, SCRATCH, GLOBAL, Invalid, Count
};
const char* fmt_name(Fmt f);

// Scalar/vector source operand encodings (9-bit "src" fields; scalar fields use the low 8 values).
namespace src {
constexpr uint16_t kVccLo = 106, kVccHi = 107, kTtmp0 = 108, kM0 = 124, kNull = 125, kExecLo = 126,
                   kExecHi = 127, kZero = 128, kIntPos1 = 129, kIntPos64 = 192, kIntNeg1 = 193,
                   kIntNeg16 = 208, kSdwa = 0xF9, kDpp16 = 0xFA, kDpp8 = 0xE9, kDpp8Fi = 0xEA,
                   kShared_base = 235, kShared_limit = 236, kPrivate_base = 237, kPrivate_limit = 238,
                   kPopsExitingWaveId = 239, kFloatHalf = 240, kFloatNegHalf = 241, kFloatOne = 242,
                   kFloatNegOne = 243, kFloatTwo = 244, kFloatNegTwo = 245, kFloatFour = 246,
                   kFloatNegFour = 247, kInvTwoPi = 248, kVccz = 251, kExecz = 252, kScc = 253,
                   kLdsDirect = 254, kLiteral = 255, kVgpr0 = 256;
}

struct Sdwa {           // second dword of an SDWA instruction
    uint8_t src0 = 0;   // register number (VGPR, or SGPR if s0 set)
    bool s0 = false, s1 = false;
    uint8_t dst_sel = 6, dst_unused = 0, src0_sel = 6, src1_sel = 6;   // 6 = DWORD
    bool clamp = false, sext0 = false, sext1 = false, neg0 = false, neg1 = false, abs0 = false, abs1 = false;
    uint8_t omod = 0;
    uint8_t sdst = 0;   // VOPC: destination SGPR when sd set
    bool sd = false;
};
struct Dpp {
    bool dpp8 = false, fi = false;
    uint8_t src0 = 0;          // VGPR
    uint16_t ctrl = 0;         // DPP16 control
    bool bound_ctrl = false, neg0 = false, abs0 = false, neg1 = false, abs1 = false;
    uint8_t row_mask = 0xF, bank_mask = 0xF;
    uint8_t sel[8] = {};       // DPP8 lane selects
};

struct Instr {
    uint32_t offset = 0;   // byte offset in the code blob
    uint8_t len = 0;       // dwords
    Fmt fmt = Fmt::Invalid;
    uint16_t op = 0;       // opcode within fmt (VOP3: the 10-bit VOP3 opcode)
    const char* name = nullptr;  // mnemonic without _e32/_e64/_sdwa/_dpp; null if unknown
    uint32_t w[4] = {};    // raw dwords (w[len-1] is the literal when has_literal)
    bool has_literal = false;
    uint32_t literal = 0;

    // ALU fields. Sources use the 9-bit src encoding (scalar formats: 8-bit values as-is).
    uint16_t dst = 0;      // vdst (VGPR number) for VALU, sdst for SALU
    uint16_t sdst = 0;     // VOP3b / VOPC-in-VOP3 scalar destination
    uint16_t src0 = 0, src1 = 0, src2 = 0;
    uint8_t abs = 0, neg = 0, neg_hi = 0;   // VOP3/VOP3P bit per source
    uint8_t opsel = 0, opsel_hi = 0;        // VOP3/VOP3P
    uint8_t omod = 0;
    bool clamp = false;
    int32_t simm16 = 0;    // SOPK / SOPP (sign-extended); SMEM/DS/MUBUF/FLAT offsets in `offset_imm`
    bool sdwa = false, dpp = false;
    Sdwa sdwa_f;
    Dpp dpp_f;

    // Memory fields.
    int32_t offset_imm = 0;
    uint16_t vaddr = 0, vdata = 0, srsrc = 0, ssamp = 0, soffset = 0, saddr = 0, sbase = 0, sdata = 0;
    uint8_t addr_extra[13] = {};   // MIMG NSA address VGPRs (after vaddr)
    uint8_t nsa = 0;               // number of NSA address registers
    uint16_t data0 = 0, data1 = 0, gds_offset1 = 0;
    bool offen = false, idxen = false, glc = false, slc = false, dlc = false, gds = false, lds = false,
         tfe = false, lwe = false, d16 = false, a16 = false, unorm = false, r128 = false, imm = false;
    uint8_t dmask = 0, dim = 0, format = 0;
    // VINTRP
    uint8_t attr = 0, attr_chan = 0;
    // EXP
    uint8_t exp_target = 0, exp_en = 0;
    bool exp_compr = false, exp_done = false, exp_vm = false;
    uint8_t exp_src[4] = {};
};

// Decodes one instruction at dword index i of `code` (n dwords). Returns false at end/truncation.
bool decode(const uint32_t* code, size_t n, size_t i, Instr& out);
// Decodes a whole code region.
std::vector<Instr> decode_all(const uint32_t* code, size_t n);

// Display text close to LLVM's spelling, for diagnostics and decoder tests.
std::string mnemonic_with_encoding(const Instr& in);  // e.g. "v_add_f32_e32", "v_mov_b32_sdwa"
std::string disasm(const Instr& in);

// Name lookup in the generated tables.
const char* op_name(Fmt f, unsigned op);

}  // namespace gpucode
