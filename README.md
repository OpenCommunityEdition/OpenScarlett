# OpenScarlett

An open-source compatibility layer for Xbox Series X|S software. It runs a title's own x86-64
code natively and replaces the Game OS user-mode libraries the title expects (`d3d12_xs.dll`,
`xgameruntime.dll`, `GameInput.dll`, …) with same-named DLLs built on Win32 and PC Direct3D 12.
On Linux the same DLLs run under Wine and vkd3d-proton. "Scarlett" is the codename of the
hardware platform the project targets.

> **Not affiliated with Microsoft.** OpenScarlett is an independent open-source project and is
> not affiliated with, endorsed by, or sponsored by Microsoft Corporation. Xbox and Xbox Series
> X|S are trademarks of Microsoft Corporation. "Scarlett" is used only to describe the hardware
> platform this project targets. Not affiliated with Focusrite.

## What this project does not do

- It ships no Microsoft software: no SDK/GDK files, no Game OS image, no system DLLs.
  Users point the build at their own SDK install; SDK-derived data is generated locally.
- It contains no keys, no decryption of packages, and no dumping tools. It runs titles from
  unpacked folders the user supplies.
- It ships no game files and no game patches; user patches are user data (docs/patches.md).
- Online services stay offline: sign-in tokens are never forged and no Microsoft service is
  contacted on a title's behalf.
- Piracy is not supported. Issues and discussions about obtaining games will be closed.

## Status

| Component | State |
|---|---|
| Launcher (`tools/xsl-run`) | Linux/Wine launcher: run directory, prefix, console-mode registry (Lockhart/Anaconda), console drive letters, back-end selection |
| Platform surface (`src/platform`) | Import-table interposition from `d3d12_xs.dll`'s `DllMain`; PLM notifications; ANSI window classes as Unicode; user patches |
| Memory (`src/xmem`) | `XMemVirtualAlloc` (64 KB-page placeholders), physical pages, title allocation hooks, GPU-visible registry |
| Gaming Runtime (`src/runtime`) | Loader protocol; API dispatch across GDK editions; task queues/async (libHttpClient), system/package/thread, persistent local storage, XUser (one offline local user), XGameSave (host folders) |
| Direct3D 12.X (`src/gfx`) | SDK object model with null put limits; device, queues, lists, resources, heaps, PSOs (incl. streams), fences, queries, frame events, `PresentX` (any queue, HDR planes), unified memory, console descriptor model, GPU page mapping, DXGI shims |
| GameInput (`src/input`) | Gamepads (XInput, incl. DualSense via SDL), keyboard and mouse devices, device callbacks, readings |
| DirectStorage (`src/storage`) | `XCRDAPI.dll`: queues, files, status arrays, fences, events, cancellation, zlib (vendored), BCPack (SDK DLL at run time) |
| Audio hardware (`src/audio`) | APU memory and a silent software audio processor (`AcpHal`), stubs for `xapu`, `chad`, `PIXEvt`, `XFrontPanelDisplay` |
| Shaders | Xbox DXIL containers rewritten to plain DXIL (checksum verified against DXC); vkd3d-proton/dxil-spirv patches for Xbox LLVM and `xbox.*` intrinsics. Shaders shipped as console GPU machine code only (no DXIL) are translated to HLSL and compiled with the PC shader compiler (`src/gpucode`, `docs/gpucode.md`): compute, pixel, vertex and mesh shaders |

**22 of 25** of Microsoft's samples, built as genuine Scarlett executables, present frames
on an Intel Iris Xe laptop, all with their intended image. The other three need mesh shaders (no DX12
Ultimate GPU here); they run their frame loops under `--backend null`. See `docs/compat.md`.

## Build (Linux)

Requirements: clang-cl, lld-link, llvm-lib, CMake ≥ 3.24, Ninja, xwin (`~/.xwin`), Wine ≥ 11,
Python 3, the licensed GDKX (April 2026 Update 5) and a checkout of Microsoft's
Xbox-GDK-Samples.

```sh
ln -s "/path/to/Microsoft GDK/260405" sdk          # gitignored; never committed
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang-cl-xwin.cmake \
      -DCMAKE_BUILD_TYPE=Release -DXSL_SAMPLES_ROOT=/path/to/Xbox-GDK-Samples
ninja -C build
tools/xsl-run build/samples/SimpleTriangle         # Esc (or View) exits
```

Run every sample and synthetic title: `tools/run-samples <outdir> [seconds]`, `tools/run-synthetic`.
Reference images: `tools/get-dxc.sh` (PC dxc), rebuild, then `tools/compare-reference` (layer vs desktop
build on real PC Direct3D 12, PSNR).
RenderDoc capture of the translated stream: `tools/xsl-run --renderdoc 300 build/samples/X` (captures in
`run/rdc/`, open them with qrenderdoc).

Useful environment variables: `XSL_LOG=info|trace|gfx=trace,…`, `XSL_LOG_FILE=…`,
`XSL_CAPTURE='Z:\path\frame.bmp@120[+count]'` (dump displayed frames; `XSL_CAPTURE_SOURCE=plane`
for the raw plane), `XSL_D3D12_DEBUG=1`, `XSL_FPS_TITLE=0`, `XSL_BACKEND=vkd3d-proton-xsl|vkd3d-proton|wine|null`,
`XSL_MEMTIME_SCALE=num/den`, `XSL_SDR_WHITE_NITS=200`, `XSL_GAMERTAG=…`, `XSL_SAVE_DIR=…`,
`XSL_DISPLAY_SIZE=1920x1080` (the client area titles see; `0` for the real window), `XSL_CLOSE_EXITS=0`,
`XSL_CONSOLE_DRIVES=0` (titles' `C:`/`T:` paths unmapped).
Diagnostics: `XSL_CBV_REALIGN=0`, `XSL_UNIFIED_DESCRIPTORS=0`, `XSL_BARRIER_FIX=0`, `XSL_PLACED_COMMITTED=1`,
`XSL_UPLOAD_CACHED=0`, `XSL_NET_TRACE=1` (log the title's socket calls), `XSL_FILE_TRACE=1` (log mapped console-drive paths).
Input: `XSL_MOUSE_CAPTURE=0`, `XSL_INPUT_TRACE=1`; on-demand frames: `XSL_CAPTURE_TRIGGER='Z:\path\trigger'`.

User patches: a `patches.txt` (or `<exe>.patches.txt`) next to a title executable is applied at
start-up, verified against the original bytes; see [docs/patches.md](docs/patches.md). The project
ships no patches; they are user data.

## Windows

The same DLLs run natively on Windows 10/11 with the system's Direct3D 12 (no Wine, no vkd3d).
Copy `build/` (the layer DLLs), `tools/xsl-run` and the SDK runtime DLLs it lists to the PC, then:

```
python tools\xsl-run path\to\Title
```

or double-click `tools\xsl-run.cmd` (it asks for the title folder), or drag a title folder onto it.

The launcher builds a run directory (symlinks, or copies on filesystems without them, refreshed
on later runs). System32 and the registry are never modified: the layer redirects the title's
loads of `xgameruntime.dll` to its own copy and answers the console-type registry read from
`XSL_CONSOLE_MODE`. On GPUs without the GPU upload heap (the Windows 10 runtime), buffers titles
map live in system memory. `XSL_D3D12_DEBUG=1` enables the debug layer and logs its messages
after failing calls.

## Hygiene

No Microsoft files enter this repository: the SDK is reached through the gitignored `sdk/`
link, generated SDK-derived code lives in `build/`, and sample executables built against the
SDK's import libraries stay in `build/`. See `AGENTS.md` and `docs/hygiene.md`.

## Licence

OpenScarlett is free software under the **GNU Affero General Public License v3.0 or later**
(`LICENSE`): the layer DLLs, the launcher and the build files. Tools, tests and generated ABI data
are MIT. Each file carries an SPDX tag; full texts are in `LICENSES/`, and third-party code and
patches keep their own licences (`third_party/README.md`).
