#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collect the two files per GDK edition that titles built with older GDKs need, from GDK
installs the user already has.

A title calls the console D3D12 vtable layout and the Gaming Runtime interface GUIDs of the GDK
edition it was built with. The layer generates both from that edition's `d3d12_xs.h` (Scarlett)
and `xgameruntime.lib` (tools/gen/gen_abi_layouts.py, gen_runtime_dispatch.py). This copies just
those two files out of each source given:

  * an installed or extracted GDK tree (a directory containing `<edition>/GXDK/...`,
    `<edition>/xbox/...` or `Microsoft GDK/<edition>/...`), or
  * a ZIP of such a tree (read in place, nothing else is extracted).

Output (licensed SDK files: keep it outside the repository, it is never committed):
    <out>/<edition>/d3d12_xs.h, <out>/<edition>/xgameruntime.lib, <out>/manifest.tsv
manifest.tsv columns: item, edition, file, bytes, sha256 (item = the <edition> folder name).
Point the build at it with -DXSL_SDK_EDITIONS_DIR=<out> (docs/sdk-editions.md).

Usage: collect_editions.py <out> <GDK dir or ZIP> [<GDK dir or ZIP> ...]
"""
import hashlib
import os
import re
import sys
import zipfile

HEADER_PREFER = [r"/xbox/include/gen9/", r"/gameKit/Include/Scarlett/", r"Scarlett"]
LIB_PREFER = [r"/xbox/lib/x64/", r"/GRDK/GameKit/Lib/amd64/", r"amd64|x64"]
EDITION = re.compile(r"(?:^|/)(\d{6})/")


def best(paths, rx, prefer):
    c = [p for p in paths if re.search(rx, p, re.I)]
    c.sort(key=lambda p: [not re.search(x, p, re.I) for x in prefer])
    return c[0] if c else None


def sources(src):
    """Yield (edition, {relative path: reader}) for each edition found in a directory or ZIP."""
    if zipfile.is_zipfile(src):
        z = zipfile.ZipFile(src)
        names = [n for n in z.namelist() if not n.endswith("/")]
        read = lambda n: z.read(n)
    else:
        names = []
        for root, _, files in os.walk(src):
            for f in files:
                if f.lower() in ("d3d12_xs.h", "xgameruntime.lib"):
                    names.append(os.path.relpath(os.path.join(root, f), src).replace(os.sep, "/"))
        read = lambda n: open(os.path.join(src, n), "rb").read()
    by_edition = {}
    for n in names:
        m = EDITION.search("/" + n)
        if m:
            by_edition.setdefault(m.group(1), []).append(n)
    for ed, paths in sorted(by_edition.items()):
        yield ed, paths, read


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, srcs = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    manifest = os.path.join(out, "manifest.tsv")
    rows = {}
    if os.path.exists(manifest):
        for line in open(manifest).read().splitlines()[1:]:
            f = line.split("\t")
            if len(f) == 5:
                rows[(f[0], f[2])] = line
    for src in srcs:
        found = 0
        for ed, paths, read in sources(src):
            hdr = best(paths, r"/?d3d12_xs\.h$", HEADER_PREFER)
            lib = best(paths, r"/?xgameruntime\.lib$", LIB_PREFER)
            for path, fname in ((hdr, "d3d12_xs.h"), (lib, "xgameruntime.lib")):
                if not path:
                    continue
                data = read(path)
                os.makedirs(os.path.join(out, ed), exist_ok=True)
                open(os.path.join(out, ed, fname), "wb").write(data)
                rows[(ed, fname)] = f"{ed}\t{ed}\t{fname}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}"
                found += 1
            print(f"{src}: edition {ed}: {'d3d12_xs.h ' if hdr else ''}{'xgameruntime.lib' if lib else ''}")
        if not found:
            print(f"{src}: no GDK edition folders with d3d12_xs.h or xgameruntime.lib found", file=sys.stderr)
    with open(manifest, "w") as m:
        m.write("item\tedition\tfile\tbytes\tsha256\n")
        m.write("\n".join(rows[k] for k in sorted(rows)) + "\n")
    print(f"{manifest}: {len({k[0] for k in rows})} edition(s)")


if __name__ == "__main__":
    main()
