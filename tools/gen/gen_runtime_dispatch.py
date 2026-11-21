#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the Gaming Runtime dispatch table from the title-side xgameruntime.lib.

Every public API in the static library is a thunk of the form (docs/provenance.md):

    lea rcx, [CLASS_GUID] ; lea rdx, [INTERFACE_GUID]  (or mov rdx, rcx)
    call QueryApiImpl
    mov rax, [obj] ; mov rax, [rax + SLOT] ; call rax   (this = obj, then the caller's args)
    mov rax, [obj] ; call [rax + 0x10]                  (Release)

This tool disassembles the library with llvm-objdump (relocations on) and emits a C++ table of
{name, class GUID, interface GUID, slot}. The output is derived from the licensed SDK and is
written to the build directory only.

Older SDKs' libraries may follow (titles built with them use their interface GUIDs and slots,
e.g. a March 2022 GDK title reaches XNetworking* through class 37e56907, gone since): their
thunks are added where the pinned SDK has no thunk on that interface slot, so every edition's
call reaches the implementation of the same name.

Usage: gen_runtime_dispatch.py <xgameruntime.lib> <llvm-objdump> <out.h> [older xgameruntime.lib ...] [--editions-dir DIR]
       (DIR: tools/sdk/collect_editions.py's output; its libraries are taken newest edition first)
"""
import os
import re
import subprocess
import sys
import tempfile


def members(lib):
    d = open(lib, "rb").read()
    assert d[:8] == b"!<arch>\n"
    off, longnames = 8, b""
    while off < len(d):
        h = d[off:off + 60]
        name = h[:16].decode().strip()
        size = int(h[48:58])
        body = d[off + 60:off + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            o = int(name[1:])
            yield longnames[o:longnames.index(b"\0", o)].decode(), body
        elif not name.startswith("/"):
            yield name.rstrip("/"), body
        off += 60 + size + (size & 1)


def guid_init(s):
    p = s.split("_")
    a, b, c, d, e = p[0], p[1], p[2], p[3], p[4]
    bytes_ = [d[0:2], d[2:4]] + [e[i:i + 2] for i in range(0, 12, 2)]
    return "{0x%s, 0x%s, 0x%s, {%s}}" % (a, b, c, ", ".join("0x" + x for x in bytes_))


def thunks(lib, objdump):
    asm = ""
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in members(lib):
            if not name.lower().endswith(".obj"):
                continue
            path = os.path.join(tmp, "m.obj")
            open(path, "wb").write(body)
            r = subprocess.run([objdump, "-d", "-r", "--no-show-raw-insn", path], capture_output=True, text=True)
            asm += r.stdout
    funcs = re.split(r"\n(?=[0-9a-f]{16} <)", asm)
    rows = []
    for f in funcs:
        m = re.match(r"[0-9a-f]{16} <([^>]+)>:", f)
        if not m:
            continue
        name = m.group(1)
        if name.startswith(("?", "$", "_")) or name in ("QueryApiImpl",):
            continue
        q = f.find("QueryApiImpl\n")
        if q < 0:
            continue
        pre = f[:q]
        regs = {}
        lines = pre.split("\n")
        for i, line in enumerate(lines):
            lm = re.search(r"leaq\s+\(%rip\), %(rcx|rdx)", line)
            if lm and i + 1 < len(lines):
                gm = re.search(r"IMAGE_REL_AMD64_REL32\s+_GUID_([0-9a-f_]+)", lines[i + 1])
                if gm:
                    regs[lm.group(1)] = gm.group(1)
            if re.search(r"movq\s+%rcx, %rdx", line) and "rcx" in regs:
                regs["rdx"] = regs["rcx"]
        sm = re.search(r"movq\s+0x([0-9a-f]+)\(%rax\), %rax", f[q:])
        if "rcx" not in regs or "rdx" not in regs or not sm:
            continue
        rows.append((name, regs["rcx"], regs["rdx"], int(sm.group(1), 16) // 8))
    return sorted(rows)


def editions_from_dir(d):
    libs = []
    for line in open(os.path.join(d, "manifest.tsv")).read().splitlines()[1:]:
        f = line.split("\t")
        if len(f) >= 3 and f[2] == "xgameruntime.lib" and f[1].isdigit():
            libs.append((int(f[1]), os.path.join(d, f[0], f[2])))
    return [p for _, p in sorted(libs, reverse=True)]


def main():
    args = sys.argv[1:]
    if "--editions-dir" in args:
        i = args.index("--editions-dir")
        extra = editions_from_dir(args[i + 1])
        args = args[:i] + args[i + 2:] + extra
    lib, objdump, out = args[0:3]
    olds = args[3:]
    rows = thunks(lib, objdump)
    have = {(r[2], r[3]): r[0] for r in rows}
    for old in olds:
        added = 0
        for r in thunks(old, objdump):
            key = (r[2], r[3])
            if key in have:
                if have[key] != r[0]:
                    if os.environ.get("XSL_GEN_VERBOSE"):
                        print(f"note: {old}: {r[0]} and {have[key]} share interface {r[2]} slot {r[3]}; keeping the newer one",
                              file=sys.stderr)
                continue
            have[key] = r[0]
            rows.append(r)
            added += 1
        if added:
            print(f"runtime dispatch: {added} thunks from {old}", file=sys.stderr)
    with open(out, "w") as o:
        o.write("// Generated by tools/gen/gen_runtime_dispatch.py from xgameruntime.lib (pinned and older SDKs). Do not commit.\n")
        o.write("#pragma once\n#include <guiddef.h>\n\n")
        o.write("struct XslRtThunk { const char* name; GUID cls; GUID iid; unsigned slot; };\n")
        o.write("static const XslRtThunk kXslRtThunks[] = {\n")
        for n, c, i, s in rows:
            o.write(f'    {{"{n}", {guid_init(c)}, {guid_init(i)}, {s}}},\n')
        o.write("};\n")
        o.write(f"static const unsigned kXslRtThunkCount = {len(rows)};\n")
    print(f"runtime dispatch: {len(rows)} thunks, {len(set(r[2] for r in rows))} interfaces", file=sys.stderr)


if __name__ == "__main__":
    main()
