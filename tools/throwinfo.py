#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Name the C++ exception types thrown by an MSVC x64 PE, from the ThrowInfo address that Wine's
+seh trace prints as info[2] (with info[3] = image base).

Usage: throwinfo.py <exe> <throwinfo VA hex> [image base hex]
"""
import struct
import sys


def main():
    path, va = sys.argv[1], int(sys.argv[2], 16)
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    if len(sys.argv) > 3:
        base = int(sys.argv[3], 16)
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((vaddr, max(vsize, rsize), rptr))

    def off(rva):
        for vaddr, size, rptr in secs:
            if vaddr <= rva < vaddr + size:
                return rptr + rva - vaddr
        raise ValueError(hex(rva))

    ti = off(va - base)
    _attrs, _unwind, _fwd, cta = struct.unpack_from("<IiiI", data, ti)
    n = struct.unpack_from("<i", data, off(cta))[0]
    for i in range(n):
        ct = struct.unpack_from("<I", data, off(cta) + 4 + 4 * i)[0]
        td = struct.unpack_from("<I", data, off(ct) + 4)[0]
        name = data[off(td) + 16:].split(b"\0")[0].decode(errors="replace")
        print(name)


if __name__ == "__main__":
    main()
