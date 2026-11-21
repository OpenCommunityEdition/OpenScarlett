# Compatibility

Microsoft's public samples built as Series X|S executables, and retail titles.

Levels: Nothing, Boots, Menus, Ingame, Playable (shadPS4 scale). For samples, "Playable" means the
sample's output matches its intended image and input works. Host: Linux, Wine 11.18, Intel Iris Xe
(Mesa ANV; feature level 12_1, no mesh shaders, no ray tracing), vkd3d-proton 3.1 with the layer's
patches (`tools/build-vkd3d-proton.sh`), DXVK 3.1.1 DXGI. Device type: Lockhart. Checked with
`tools/run-samples` (one captured frame per sample) and `tools/run-synthetic`.

Summary (2026-11-21): **22 of 25** built samples present frames and all 22 show their intended image
(9 Playable, 13 Ingame with the image checked but input not exercised). The 5 `_nodebug` builds also match.
The other 3 need mesh shaders (no DirectX 12 Ultimate GPU here) and run their frame loops under null graphics.

| Sample | Level | Exercises | Notes |
|---|---|---|---|
| SimpleTriangle | Playable | device, queue, list, PSO, PresentX | `_nodebug` build also correct |
| SimpleTexture | Playable | WIC upload, CopyTextureRegionX | |
| SimpleLighting | Playable | constant buffers, depth | |
| SimpleCompute | Playable | compute PSO, root signature serialization | |
| SimpleDeviceAndSwapChain | Playable | DDS loading, SpriteBatch | |
| SimpleDynamicResources | Playable | SM 6.6 heap-indexed resources | needs vkd3d-proton (Wine's builtin vkd3d lacks createHandleFromHeap) |
| SimpleBezier | Playable | tessellation (HS/DS) | `_nodebug` build (inlined Xbox-format CBV) also correct |
| SimpleMSAA | Playable | MSAA, ResolveSubresource | needed 16-byte constant-buffer realignment |
| SimpleInstancing | Playable | instancing, `sign()` | Xbox DXC emits `llvm.copysign` (dxil-spirv patch) |
| SimpleHDR | Ingame | R9G9B9E5 back buffer, linear-P3 HDR plane | format stand-in; HDR plane mapped to SDR; UAV in a non-shader-visible heap (console descriptor model); image checked, input not exercised |
| SimplePBR | Ingame | DXTK PBR, HDR, tonemap | console descriptor model; image checked, input not exercised |
| HDR10 | Ingame | HDR swap chain values | display reports SDR (`XDisplayTryEnableHdrMode`); image checked, input not exercised |
| HistogramCS | Ingame | compute histogram | image checked, input not exercised |
| ComputeParticles | Ingame | compute + graphics | image checked, input not exercised |
| ExecuteIndirect | Ingame | command signatures, ExecuteIndirect | image checked, input not exercised |
| FramePacing | Ingame | frame events, compute-queue PresentX, `__XB_MemTime` GPU load, query heaps | ~48 fps with the default loads; frame statistics drive the frame-time, CPU and flip graphs (GPU-timestamp bars still empty; reported latency not yet validated); image checked, input not exercised |
| AsynchronousProgramming | Ingame | XTaskQueue / XAsync | image checked, input not exercised |
| LocalStorage | Ingame | XPersistentLocalStorage | image checked, input not exercised |
| SimpleDirectStorageCombo | Ingame | XCRDAPI DirectStorage: file/memory queues, zlib, cancellation, status arrays, fences, events, multiple queues | every test reports Success (the desktop GPU-decompression test is desktop-only); the sample writes a 5 GB data file to `D:`; image checked, input not exercised |
| GameSaveCombo | Ingame | XUser sign-in, XGameSave containers/blobs | gamer picture not implemented; image checked, input not exercised |
| AdvancedESRAM | Ingame | XMem reservations, page pools, CreatePlacedResourceX, CopyPageMappingsBatchX | renders correctly (the scene needed console rsqrt clamping, see below); image checked, input not exercised |
| HlslCompile | Ingame | stripped shaders, PDB variants, run-time compiler API | all six variants render: programs recovered from ILDB, from full PDBs, or recompiled from slim PDB sources with the SDK compiler; three shaders come from the sample's own MyHlslCompiler tool, built and run at build time; image checked, input not exercised |
| SimpleMeshlet | Boots | mesh shaders | host GPU has none (DirectX 12 Ultimate needed); PSO creation fails cleanly. Runs its frame loop (~57 fps) under `--backend null` |
| DynamicCubeMap | Boots | mesh/amplification path | as above; runs under `--backend null` |
| PointSprites | Boots | mesh path | as above; the title then crashes on the null PSO. Runs under `--backend null` |
| Gamepad | Nothing | | needs the GameInput v4 NuGet headers, not in the GDKX |

Synthetic titles (`tests/synthetic`): `runtime_smoke` (runtime loader, task queues),
`xmem_smoke` (XMem, physical pages), `save_smoke` (sign-in and a full save round trip, sync and
async), `memtime_smoke` (`__XB_MemTime` through the layer), `dstorage_smoke` (memory requests: plain,
zlib, BCPack, zlib+BCPack; error records; cancellation; fence signals) — all pass.

## Retail titles

| Title | GDK edition | Status | Notes |
|---|---|---|---|
| Minecraft Preview 1.18.3064.0 (Series X\|S, unmodified executable) | GXDK 220300 (March 2022) | **Playable**: worlds are created, saved and played (Linux and natively on Windows 10, RTX 5090, ~60 FPS in-world, no D3D12 errors), silent | Needs the 220300 edition files (docs/sdk-editions.md). Input: DualSense/XInput pads, keyboard and mouse through GameInput (mouse captured while focused). Known gaps: no audio (silent ACP); speech synthesis and game streaming unimplemented; CPU writes to mapped GPU-only textures stay on a CPU shadow (only render targets seen so far). Fixed on the way: constants bound at 16-byte offsets in default-heap buffers were not re-aligned (skin preview and loading-screen logo invisible); texture copies with 256-byte-aligned buffer footprints removed the device on Windows when a world loaded; CPU reads of write-combined upload memory held the world at ~3 FPS; world creation failed for standard (non-elevated) users on Windows because the title's console `C:` storage was the real system drive. |

## Back-end findings

- **Constant-buffer alignment**: console `D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT` is 16 (PC 256).
  Unaligned root CBVs are re-pointed into a 256-aligned ring (`XSL_CBV_REALIGN=0` disables). The ring
  grows per frame in 16 MB chunks (Minecraft's worlds bind well over 4096 such constants a frame).
- **Texture placement alignment**: console buffer footprints for `CopyTextureRegion` may sit at 256-byte
  offsets; PC D3D12 requires 512 and fails the command list (device removed at the next submission; Vulkan
  does not enforce it). Misaligned footprints are rebased to the aligned offset below and the copy region
  shifted by whole rows or by texels within the row padding.
- **Upload memory**: console memory is CPU-cached; a PC UPLOAD heap is write-combined (or device-local
  BAR memory under vkd3d-proton), where CPU reads are uncached. The CBV re-alignment reads title buffers
  at bind time, so UPLOAD heaps are created as write-back system memory (`XSL_UPLOAD_CACHED=0` disables).
- **Console drives**: titles address storage by drive letter (Minecraft: `C:/Users/<xuid>/games/com.mojang`,
  `C:/` as its storage root, `t:/` for temporary data). Under Wine C: is the prefix's private drive; on Windows
  it is the system drive (standard users cannot create folders under `C:\Users`) and T: does not exist. The
  title modules' file calls (Win32, CRT, free-space queries, handle renames) map `C:` (Windows only) and `T:` to
  `C:\users\Public\OpenScarlett\<title>\Drives\C|T`; paths in the title, layer and temp folders pass through.
- **Display size**: titles size display planes from their window's client area, which on a console is the
  output resolution. A PC window's client area is smaller (title bar, taskbar), and planes of that size
  next to 1080p targets meant mismatched resolves and copies; the title's `GetClientRect` reports the
  console size and the swap chain stretches it onto the window.
- **Descriptor heaps**: on the console every CBV/SRV/UAV heap is GPU-visible and descriptor tables may
  point into any of them; samples rely on it. The layer keeps one shader-visible heap and gives each
  title heap a range (`XSL_DESCRIPTOR_HEAP_SIZE`, `XSL_UNIFIED_DESCRIPTORS=0`).
- **Retail (`D3DCOMPILE_NO_DEBUG`) descriptors**: d3d12_xs.h inlines `CreateConstantBufferView` (writes a
  16-byte Xbox buffer descriptor) and `CopyDescriptorsSimple` (raw copies at 32/112/128 bytes per
  descriptor). Title CPU handles therefore point into layer-owned, console-layout descriptor memory:
  layer-created views are tags naming immutable PC records, inline Xbox CBVs are decoded, and dirty
  slots (write watch) are synced to PC descriptors before each `ExecuteCommandLists`
  (`XSL_SHADOW_DESCRIPTORS=0` disables). The `_nodebug` sample builds exercise it.
- **Barrier states**: console drivers tolerate a `StateBefore` that is not the resource's state (the
  samples' `Present()` after a compute pass). Within one command list the layer starts such a
  transition from the state the previous transition left (`XSL_BARRIER_FIX=0` disables).
- **LLVM intrinsics in Xbox DXIL**: `llvm.copysign` for `sign()` and other math intrinsics that
  upstream dxil-spirv drops. Unknown value-producing intrinsics now fail the conversion loudly.
- **Stripped shaders**: containers whose DXIL part holds only native code are translated from their debug
  program: the ILDB part, a full PDB named by ILDN, or a slim PDB (`/Zs`) recompiled from its embedded
  sources by the SDK's `dxcompiler_xs.dll` (copied from the user's SDK at run time, as `xg_xs.dll` is).
  Retail titles ship neither; for them this needs a native-code shader recompiler (not implemented).
- **Console rsqrt**: AdvancedESRAM's models have all-zero normals; `normalize(0)` is NaN under IEEE rules
  (every lit pixel black) but the scene renders on the console, so console shaders evidently clamp
  rsqrt (AMD `v_rsq_clamp`-style fast math). vkd3d-proton's `FIXUP_RSQRT_INF_NAN` quirk is enabled for
  every shader (patch `vkd3d-proton-openscarlett`). Inferred from this sample, not from documentation.
- **Xbox intrinsics**: `xbox.MemTime` (`__XB_MemTime`) → `OpReadClockKHR` (device scope when the GPU
  has it, else subgroup scope, which on Intel is per thread and not comparable across dispatches);
  `XSL_MEMTIME_SCALE=num/den` converts host clocks to console GPU clocks (1565 MHz on Lockhart).
- **Presentation**: swap chains need a direct queue; `PresentX` on a compute queue goes through a
  layer-owned direct queue. HDR planes (linear P3, ST.2084 Rec.2020, scRGB) are mapped to SDR with a
  reference white of `XSL_SDR_WHITE_NITS` (default 200).
- **DirectStorage**: console DirectStorage is `DStorageGetFactory` in `XCRDAPI.dll`; destinations are
  title memory (CPU addresses or physical pages). The layer's `XCRDAPI.dll` runs each queue on a worker
  thread with Win32 reads, zlib (vendored 1.3.1) and BCPack (the SDK's `BCPack.dll`, copied from the user's
  SDK at run time); swizzle modes are not implemented.
- **Unified memory, CPU side**: console titles `Map` DEFAULT-heap buffers. Such buffers are created
  CPU-visible on PC (GPU upload heap, or write-back system memory on UMA GPUs; `XSL_MAPPABLE_DEFAULT=0`).
- **GPU page mapping**: page pools become PC heaps, placed resources on reserved address space become
  reserved resources, page mappings become `UpdateTileMappings` on the same queue.
- **Window titles**: the FPS suffix is written without `WM_GETTEXT`, which deadlocks against a window
  thread that waits for the render thread.
- **Null graphics** (`xsl-run --backend null`, `XSL_NULL_GRAPHICS=1`): objects and memory are real, PSOs are
  placeholders (no shader translation), draws/dispatches are dropped and `PresentX` only paces. Titles whose
  graphics cannot be translated yet keep running their frame loops.
- **Intermediate passes**: `XSL_DBG_PRESENT_RES=k` presents the k-th render-target texture (with
  `XSL_DBG_ALL_TEX=1`, any 2D texture) instead of the title's plane, to inspect passes; RenderDoc capture of
  titles using unified (host) memory did not complete on this host.
- **Captures**: `XSL_CAPTURE` captures the displayed buffer (opaque pixels);
  `XSL_CAPTURE_SOURCE=plane` captures the title's raw plane.
