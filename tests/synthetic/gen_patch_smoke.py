#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write patch_smoke.patches.txt for the patch_smoke synthetic title: finds its markers in the
built executable and emits one group of each kind (applied, mismatched, disabled, RVA form).

Usage: gen_patch_smoke.py <patch_smoke.exe> <out patches.txt>
"""
import struct
import sys


def main():
    exe, out = sys.argv[1], sys.argv[2]
    d = open(exe, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    secs = [struct.unpack_from("<IIII", d, pe + 24 + opt + i * 40 + 8) for i in range(nsec)]

    def rva(marker):
        off = d.find(marker.encode() + b"\0")
        assert off >= 0 and d.find(marker.encode() + b"\0", off + 1) < 0, marker
        for vsz, va, rsz, raw in secs:
            if raw <= off < raw + rsz:
                return va + off - raw
        raise SystemExit(f"{marker} not in a section")

    def code_rva(pattern):
        off = d.find(pattern)
        assert off >= 0 and d.find(pattern, off + 1) < 0, pattern
        for vsz, va, rsz, raw in secs:
            if raw <= off < raw + rsz:
                return va + off - raw
        raise SystemExit("code pattern not in a section")

    hexs = lambda s: " ".join(f"{b:02X}" for b in s.encode())
    o, p = "XSLPATCH_APPLY_ORIGINAL", "XSLPATCH_APPLY_PATCHED!"
    lines = ["# Generated for the patch_smoke synthetic title (tests/synthetic/gen_patch_smoke.py).", "",
             "[Apply]", f"0x{base + rva(o):X}: {hexs(o)} -> {hexs(p)}", "",
             "[Mismatch]  # original bytes deliberately wrong: the group must be skipped",
             f"0x{base + rva('XSLPATCH_MISMT_ORIGINAL'):X}: {hexs('XSLPATCH_MISMT_WRONGXXX')} -> {hexs('XSLPATCH_MISMT_PATCHED!')}", "",
             "[Disabled] off",
             f"0x{base + rva('XSLPATCH_DISAB_ORIGINAL'):X}: {hexs('XSLPATCH_DISAB_ORIGINAL')} -> {hexs('XSLPATCH_DISAB_PATCHED!')}", "",
             "[RVA]",
             f"rva 0x{rva('XSLPATCH_RVAXX_ORIGINAL'):X}: {hexs('XSLPATCH_RVAXX_ORIGINAL')} -> {hexs('XSLPATCH_RVAXX_PATCHED!')}", "",
             "[Code]  # answer(): mov eax, 1234567 -> mov eax, 7654321",
             f"0x{base + code_rva(bytes([0xB8]) + (1234567).to_bytes(4, 'little') + bytes([0xC3])):X}: B8 87 D6 12 00 -> B8 B1 CB 74 00", ""]
    open(out, "w").write("\n".join(lines))


if __name__ == "__main__":
    main()
