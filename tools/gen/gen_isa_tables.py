#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the GFX10 (Xbox Series X|S) opcode tables for src/gpucode from LLVM's AMDGPU disassembler.

Every opcode of every instruction format is probed with a fixed encoding and benign register
operands, `llvm-mc --disassemble --mcpu=gfx1013` names it, and the mnemonic is recorded. Opcodes
LLVM rejects stay unnamed. The output is a list of facts (format, opcode number, mnemonic); no
LLVM source is copied. Provenance: docs/provenance.md, "GPU instruction tables".

usage: gen_isa_tables.py <out.inc> [llvm-mc]
"""
import re
import subprocess
import sys

SEP = [0xBEE303FF, 0x13579BDF]  # s_mov_b32 s99, 0x13579bdf: between probes
SEP_TEXT = "s_mov_b32 s99, 0x13579bdf"


def probes():
    """(format, opcode, [dwords] or [[dwords], [dwords], ...] variants tried in order)"""
    for op in range(0x60):  # SOP2; opcodes >= 0x60 are the SOPK space
        yield "SOP2", op, [0x80000000 | op << 23 | 2 << 16 | 4 << 8 | 6]
    for op in range(32):
        yield "SOPK", op, [0xB0000000 | op << 23 | 2 << 16 | 0x10]
    for op in range(256):
        yield "SOP1", op, [0xBE800000 | 2 << 16 | op << 8 | 4]
    for op in range(128):
        yield "SOPC", op, [0xBF000000 | op << 16 | 4 << 8 | 6]
    for op in range(128):
        yield "SOPP", op, [0xBF800000 | op << 16]
    for op in range(256):
        yield "SMEM", op, [0xF4000000 | op << 18 | 4 << 6 | 1, 0x7D << 25]
    for op in range(62):
        yield "VOP2", op, [[op << 25 | 1 << 17 | 2 << 9 | 0x100], [op << 25]]
    for op in range(256):
        yield "VOP1", op, [[0x7E000000 | 1 << 17 | op << 9 | 0x100], [0x7E000000 | op << 9]]
    for op in range(256):
        yield "VOPC", op, [0x7C000000 | op << 17 | 2 << 9 | 0x100]
    for op in range(1024):
        yield "VOP3", op, [0xD4000000 | op << 16 | 1, 0x100 | 0x101 << 9 | 0x102 << 18]
    for op in range(128):
        yield "VOP3P", op, [0xCC000000 | op << 16 | 0x4000 | 1, 0x100 | 0x101 << 9 | 0x102 << 18 | 3 << 27]
    for op in range(4):
        yield "VINTRP", op, [0xC8000000 | 1 << 18 | op << 16 | 0]
    for op in range(256):
        yield "DS", op, [[0xD8000000 | op << 18, w1] for w1 in (
            0 | 1 << 8 | 2 << 16 | 3 << 24, 0 | 1 << 8 | 2 << 16, 0 | 1 << 8 | 3 << 24, 0 | 1 << 8, 0 | 3 << 24, 0,
            0 | 2 << 8 | 4 << 16 | 6 << 24, 0 | 2 << 8 | 4 << 16, 0 | 2 << 8 | 6 << 24, 0 | 2 << 8)]
    for op in range(256):
        yield "MUBUF", op, [0xE0000000 | op << 18, 0 | 1 << 8 | 1 << 16 | 0x80 << 24]
    for op in range(16):
        yield "MTBUF", op, [0xE8000000 | 1 << 19 | (op & 7) << 16, 0 | 1 << 8 | 1 << 16 | (op >> 3) << 21 | 0x80 << 24]
    for op in range(256):
        w0 = 0xF0000000 | (op & 0x7F) << 18 | (op >> 7)
        yield "MIMG", op, [[w0 | 0xF << 8 | 1 << 3, 0 | 4 << 8 | 2 << 16 | 1 << 21],
                           [w0 | 0xF << 8 | 1 << 3, 4 | 2 << 16],
                           [w0 | 0x1 << 8 | 1 << 3 | 1 << 13, 4 | 2 << 16],
                           [w0 | 0xF << 8 | 1 << 12 | 1 << 15, 4]]
    for seg, name in ((0, "FLAT"), (1, "SCRATCH"), (2, "GLOBAL")):
        for op in range(128):
            yield name, op, [0xDC000000 | op << 18 | seg << 14, 0 | 2 << 8 | 0x7D << 16 | 3 << 24]


def main():
    out = sys.argv[1]
    mc = sys.argv[2] if len(sys.argv) > 2 else "llvm-mc"
    allp = [(f, o, w if isinstance(w[0], list) else [w]) for f, o, w in probes()]
    named = {}
    for v in range(max(len(w) for _, _, w in allp)):
        todo = [(f, o, w[v]) for f, o, w in allp if (f, o) not in named and v < len(w)]
        for (f, o, _), n in zip(todo, run(mc, todo)):
            if n:
                named[(f, o)] = n
    emit(out, [(f, o, named.get((f, o))) for f, o, _ in allp])


def run(mc, plist):
    lines = []
    for _, _, words in plist:
        lines.append(",".join("0x%02x" % b for w in words + [0xBF800000] * 2 for b in w.to_bytes(4, "little")))
        lines.append(",".join("0x%02x" % b for w in SEP for b in w.to_bytes(4, "little")))
    r = subprocess.run([mc, "--disassemble", "--triple=amdgcn", "--mcpu=gfx1013"], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    # Group stdout by separator. Rejected probes print nothing, so map groups via stderr line numbers.
    bad = {int(m.group(1)) for m in re.finditer(r"<stdin>:(\d+):1: warning: invalid instruction encoding", r.stderr)}
    groups, cur = [], []
    for l in r.stdout.splitlines():
        t = l.strip()
        if not t or t.startswith("."):
            continue
        if t == SEP_TEXT:
            groups.append(cur)
            cur = []
        else:
            cur.append(t)
    names = []
    gi = 0
    for i, _ in enumerate(plist):
        if (2 * i + 1) in bad:
            names.append(None)
            gi += 1  # the separator line still printed
            continue
        g = groups[gi]
        gi += 1
        names.append(g[0].split()[0] if g else None)
    if gi != len(groups):
        sys.exit("probe/output mismatch: %d groups for %d probes" % (len(groups), gi))
    return [None if (n == "s_nop" and not (f == "SOPP" and o == 0)) else n for (f, o, _), n in zip(plist, names)]


def emit(out, entries):
    table = {}
    for fmt, op, n in entries:
        if n:
            n = re.sub(r"_(e32|e64|sdwa|dpp)$", "", n)
            table.setdefault(fmt, []).append((op, n))
    with open(out, "w") as f:
        f.write("// SPDX-License-Identifier: AGPL-3.0-or-later\n")
        f.write("// Generated by tools/gen/gen_isa_tables.py from LLVM's AMDGPU disassembler (gfx1013). Do not edit.\n")
        f.write("// Format, opcode, mnemonic. Provenance: docs/provenance.md, \"GPU instruction tables\".\n")
        total = 0
        for fmt in sorted(table):
            for op, n in table[fmt]:
                f.write('XSL_ISA_OP(%s, %d, "%s")\n' % (fmt, op, n))
                total += 1
    print("%d opcodes in %d formats -> %s" % (total, len(table), out))
    for fmt in sorted(table):
        print("  %-8s %d" % (fmt, len(table[fmt])))


if __name__ == "__main__":
    main()
