#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile DirectXTK12's shaders for Scarlett with the GDKX dxc.exe under Wine.

Replays the `call :CompileShader%1 <file> <stage> <entry>` and
`call :CompileComputeShader%1 <file> <entry>` lines of Src/Shaders/CompileShaders.cmd in
"gxdk scarlett" mode, producing Compiled/XboxGamingScarlett<file>_<entry>.inc headers.

Usage: dxtk_shaders.py <DirectXTK12/Src/Shaders> <dxc.exe> <outdir> <wineprefix> [desktop]
"desktop" builds the PC set instead (CompileShaders.cmd "dxil" mode: unprefixed headers, the PC dxc),
for the reference builds of the samples.
"""
import concurrent.futures as cf
import os
import re
import subprocess
import sys


def jobs(shader_dir):
    text = open(os.path.join(shader_dir, "CompileShaders.cmd"), encoding="utf-8", errors="replace").read()
    for m in re.finditer(r"^call :CompileShader%1 (\w+) (vs|ps|cs|gs|hs|ds) (\w+)", text, re.M):
        yield m.group(1) + ".fx", m.group(2), m.group(3), m.group(1)
    for m in re.finditer(r"^call :CompileComputeShader%1 (\w+) (\w+)", text, re.M):
        yield m.group(1) + ".hlsl", "cs", m.group(2), m.group(1)


def main():
    shader_dir, dxc, out, prefix = sys.argv[1:5]
    desktop = len(sys.argv) > 5 and sys.argv[5] == "desktop"
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, WINEPREFIX=prefix, WINEDEBUG="-all")
    todo = []
    for src, stage, entry, base in jobs(shader_dir):
        target = os.path.join(out, f"{'' if desktop else 'XboxGamingScarlett'}{base}_{entry}.inc")
        if os.path.exists(target) and os.path.getmtime(target) > os.path.getmtime(os.path.join(shader_dir, src)):
            continue
        cmd = ["wine", dxc, src, "-nologo", "/Ges", "/Zpc", "/Qstrip_reflect", "/Qstrip_debug", f"/T{stage}_6_0",
               "-HV", "2021", f"/E{entry}", f"/Fh{target}", f"/Vn{base}_{entry}"]
        if not desktop:
            cmd.insert(-3, "/D__XBOX_PER_THREAD_SCRATCH_SIZE_LIMIT_IN_BYTES=0")
        todo.append((cmd, target))
    failed = 0
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        futs = {ex.submit(subprocess.run, c, cwd=shader_dir, env=env, capture_output=True, text=True): t for c, t in todo}
        for f in cf.as_completed(futs):
            r = f.result()
            if r.returncode != 0 or not os.path.exists(futs[f]):
                failed += 1
                print(f"FAILED {futs[f]}\n{r.stdout}{r.stderr}", file=sys.stderr)
    print(f"dxtk shaders: {len(todo) - failed} compiled, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
