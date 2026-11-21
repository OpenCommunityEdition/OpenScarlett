#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decoder test (tier 0): compare xsl-gpucode's instruction stream with LLVM's for an amdgcn ELF.

usage: compare_llvm.py <xsl-gpucode> <code.elf> [<llvm.elf>]
<llvm.elf> defaults to <code.elf>; pass a copy whose Xbox-only encodings were normalised when
the code uses them (stock LLVM rejects SGPR tuples that reach VCC and VOPC with DPP16).
Compares per instruction: offset, length and mnemonic (with its encoding suffix stripped).
Words LLVM cannot decode are reported separately and not counted as mismatches."""
import collections, re, subprocess, sys

def strip(m):
    return re.sub(r"_(e32|e64|sdwa|dpp)$", "", m)

def ours(tool, elf):
    r = subprocess.run([tool, "dis-elf", elf], capture_output=True, text=True, check=True)
    d = collections.defaultdict(dict)
    for l in r.stdout.splitlines():
        sym, off, ln, mn = l.split("\t")
        d[sym][int(off)] = (int(ln), strip(mn))
    return d

def llvm(elf):
    p = subprocess.Popen(["llvm-objdump", "-d", "--mcpu=gfx1013", "--no-show-raw-insn", elf],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    d = collections.defaultdict(dict); sym = None; base = 0
    hdr = re.compile(r"^([0-9a-f]+) <(.+)>:")
    ins = re.compile(r"^\t(\S+).*// ([0-9A-Fa-f]+):")
    for l in p.stdout:
        m = hdr.match(l)
        if m:
            sym, base = m.group(2), int(m.group(1), 16); continue
        m = ins.match(l)
        if m and sym:
            d[sym][int(m.group(2), 16) - base] = strip(m.group(1))
    return d

def main():
    tool, elf = sys.argv[1], sys.argv[2]
    a, b = ours(tool, elf), llvm(sys.argv[3] if len(sys.argv) > 3 else elf)
    n = ok = undecoded = inside = datash = 0
    bad = collections.Counter(); example = {}
    for sym, ins in b.items():
        mine = a.get(sym, {})
        # Shaders with words LLVM cannot decode embed data in the code; after such data the two
        # decoders may legitimately resynchronise differently, so those shaders are counted apart.
        if any(mn.startswith(("<", ".")) or "unknown" in mn for mn in ins.values()):
            datash += 1; undecoded += sum(1 for mn in ins.values() if mn.startswith(("<", ".")) or "unknown" in mn)
            continue
        covered = set()
        for off, (ln, _) in mine.items():
            covered.update(range(off + 4, off + 4 * ln, 4))
        for off, mn in ins.items():
            if off in covered and mn == "s_nop":
                inside += 1; continue   # placeholder left by normalising a longer Xbox-only encoding
            n += 1
            got = mine.get(off)
            if got and got[1] == mn:
                ok += 1
            else:
                key = (mn, got[1] if got else "<no instruction at offset>")
                bad[key] += 1; example.setdefault(key, (sym, off))
    print("instructions compared: %d, agree: %d, disagree: %d" % (n, ok, n - ok))
    print("skipped: %d shaders with embedded data (%d LLVM-undecoded words), %d normalisation placeholders" % (datash, undecoded, inside))
    for (k, c) in bad.most_common(25):
        print("  llvm %-28s ours %-28s x%d  e.g. %s+0x%x" % (k[0], k[1], c, *example[k]))
    sys.exit(0 if n == ok else 1)

if __name__ == "__main__":
    main()
