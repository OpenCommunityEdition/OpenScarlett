# Status (2026-11-21)

Where the implementation stands against the project's test ladder and its first-phase exit
criteria. Retail-title results are in `docs/compat.md`.

## Test ladder

| Step | Content | State |
|---|---|---|
| 0 | Unit tests, synthetic callers | `tests/unit` (DXBC checksum), `tests/synthetic` (runtime, XMem, saves, MemTime, DirectStorage) pass; ABI probes (`tools/abi`) |
| 1 | Oracle programs against the PC Gaming Runtime | Not done: needs a Windows install |
| 2 | Host programs loading `xg_xs.dll` etc. | `xg_xs.dll` loads in titles (copied from the user's SDK at run time); SDK `dxc.exe` drives every shader build |
| S1 | SimpleTriangle | Runs, correct image |
| S2 | FramePacing | Runs (frame events, compute-queue `PresentX`, `__XB_MemTime` GPU loads, query heaps, frame statistics) |
| S3 | SimpleDirectStorageCombo | Runs; all of its DirectStorage tests pass (`XCRDAPI.dll`: file/memory sources, zlib, status, fences, events, cancellation); BCPack via the SDK's BCPack.dll (`dstorage_smoke`); swizzle not implemented |
| S4 | AdvancedESRAM | Runs and renders correctly (XMem reservations, page pools, `CreatePlacedResourceX`, page mappings; console rsqrt clamping) |
| S5 | HlslCompile | Runs, all six variants correct: stripped programs recovered from ILDB, full PDBs, or slim PDBs recompiled from source (SDK compiler at run time). Shaders with native code only and no PDB (retail) still need the phase-6 recompiler; serialized ACO pipelines get placeholder PSOs |
| S6 | DXR / mesh sample | SimpleMeshlet, DynamicCubeMap, PointSprites build; need a DirectX 12 Ultimate GPU (this host has none) |
| S7 | Sample rebuilt with `D3DCOMPILE_NO_DEBUG` | Built (`<name>_nodebug`: SimpleTriangle, SimpleTexture, SimpleLighting, SimpleBezier, ExecuteIndirect) and measured: inlined `CreateConstantBufferView` wrote Xbox buffer descriptors into PC descriptor memory (heap corruption, crash) and inlined `CopyDescriptorsSimple` worked only because the PC descriptor size happens to be 32 bytes. Fixed with console-layout descriptor memory and a decoder (`src/gfx/xs_descriptors.cpp`); all five match their normal builds |
| Others | 15 further samples | See `docs/compat.md`: 22 of 25 present frames |

## Retail titles

Titles built with older GDK editions are supported through per-edition ABI layouts
(`docs/sdk-editions.md`). Per-title results, including what works and what does not, are in
`docs/compat.md`.

## First-milestone criteria

| Criterion | State |
|---|---|
| S1–S4 run under Wine on the laptop | S1–S4 run and render correctly |
| …and match desktop-build reference images | Reference pipeline in place (`tests/desktop`, `tools/compare-reference`, PC dxc via `tools/get-dxc.sh`): SimpleTriangle vs SimpleTriangleDesktop on real PC D3D12, PSNR 49 dB. This samples checkout has desktop projects only for SimpleTriangle (and SimpleDirectStorageCombo, which needs the DirectStorage NuGet package); the other samples are Xbox-only and checked by eye |
| S5 and S7 measured | Both measured and passing |
| Import coverage | Every import of the 24 samples resolves (layer DLLs, interposed platform APIs) |
| Runtime classes | Task queues/async, system, package, thread, networking hint, display, local storage, XUser, XGameSave; 87 of 419 APIs; the rest return `E_NOTIMPL` with a log line |
| XMem | Virtual (64 KB placeholders), physical pages, title hooks, GPU-visible registry |
| Null and D3D12 back ends | D3D12 back end (vkd3d-proton, stock vkd3d-proton, Wine builtin); null graphics (`--backend null`): the mesh-shader samples run their frame loops on this GPU |
| Trace capture and replay | Command tracing (`XSL_LOG=gfx.cmd=trace`), frame capture (`XSL_CAPTURE`), and RenderDoc capture of the translated stream (`xsl-run --renderdoc N`, replayed in qrenderdoc); console-level (pre-translation) replay not done; scripted replay (qrenderdoc --python) hangs on this host |

## Host limits on this laptop

Intel Iris Xe: feature level 12_1, no mesh shaders, no ray tracing, no device-scope shader clock.
Mesh/RT samples and faithful `__XB_MemTime` comparisons across dispatches need another GPU.
