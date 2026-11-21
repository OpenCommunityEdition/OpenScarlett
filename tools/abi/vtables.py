#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""List the console D3D12 function tables (vtables) of a d3d12_xs.h, slot by slot.

d3d12_xs.h declares each interface as `struct <Name>_Function_Table { <Base>_Function_Table X;
RET (D3DAPI* Method)(...); ... }`. This flattens the base tables and prints, per table, the
method names in slot order. With two headers it prints the differences per table.

Usage: vtables.py A/d3d12_xs.h [B/d3d12_xs.h]
"""
import re
import sys


def tables(path):
    src = open(path, errors="ignore").read()
    raw = {}
    for m in re.finditer(r"struct (\w+)_Function_Table\s*\{(.*?)\n\s*\};", src, re.S):
        name, body = m.group(1), m.group(2)
        items = []
        for line in re.finditer(r"^\s*(?:(\w+)_Function_Table\s+\w+\s*;|[\w\s\*]+?\(\s*D3DAPI\s*\*\s*(\w+)\s*\))", body, re.M):
            if line.group(1):
                items.append(("base", line.group(1)))
            elif line.group(2):
                items.append(("fn", line.group(2)))
        raw[name] = items
    flat = {}
    def get(n):
        if n in flat:
            return flat[n]
        out = []
        for kind, v in raw.get(n, []):
            out += get(v) if kind == "base" else [v]
        flat[n] = out
        return out
    for n in raw:
        get(n)
    return flat


def main():
    a = tables(sys.argv[1])
    if len(sys.argv) == 2:
        for n, fns in a.items():
            print(f"{n}: {len(fns)} slots")
            for i, f in enumerate(fns):
                print(f"  {i:3} {f}")
        return
    b = tables(sys.argv[2])
    for n in sorted(set(a) | set(b)):
        fa, fb = a.get(n), b.get(n)
        if fa == fb:
            continue
        if fa is None or fb is None:
            print(f"{n}: only in {'B' if fa is None else 'A'}")
            continue
        first = next((i for i, (x, y) in enumerate(zip(fa, fb)) if x != y), min(len(fa), len(fb)))
        print(f"{n}: A {len(fa)} slots, B {len(fb)} slots, first difference at slot {first}")
        print(f"   A only: {[f for f in fa if f not in fb]}")
        print(f"   B only: {[f for f in fb if f not in fa]}")


if __name__ == "__main__":
    main()
