#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create symlinks so #include spellings resolve on a case-sensitive file system.

Scans every #include in the given source trees; for each included file name that does not
exist with that exact spelling in the header directories but does exist case-insensitively,
creates <outdir>/<spelling> -> <real file>.

Usage: case_links.py <outdir> --headers DIR... --sources DIR...
"""
import os
import re
import sys

args = sys.argv[1:]
out = args[0]
hdr_dirs = args[args.index("--headers") + 1:args.index("--sources")]
src_dirs = args[args.index("--sources") + 1:]
os.makedirs(out, exist_ok=True)

index = {}
exact = set()
for d in hdr_dirs:
    for root, _, files in os.walk(d):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), d)
            exact.add(rel)
            index.setdefault(rel.lower(), os.path.join(root, f))

inc_re = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
wanted = set()
for d in src_dirs + hdr_dirs:
    for root, _, files in os.walk(d):
        for f in files:
            if not f.lower().endswith((".h", ".hpp", ".inl", ".cpp", ".c", ".hlsl", ".hlsli", ".fx", ".fxh", ".inc")):
                continue
            try:
                data = open(os.path.join(root, f), "rb").read()
            except OSError:
                continue
            for m in inc_re.finditer(data):
                wanted.add(m.group(1).decode(errors="replace").replace("\\", "/"))
made = 0
for w in sorted(wanted):
    if w in exact:
        continue
    real = index.get(w.lower())
    if not real:
        continue
    dst = os.path.join(out, w)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    os.symlink(real, dst)
    made += 1
print(f"case links: {made}")
