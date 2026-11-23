# Console GPU code translation (`src/gpucode`)

Titles may ship shaders as console GPU machine code only (the DXIL is stripped). The layer then
translates the machine code to HLSL and compiles it with Microsoft's PC shader compiler
(`dxcompiler.dll` from the DirectXShaderCompiler release, placed next to the layer DLLs by
`tools/xsl-run`; `tools/get-dxc.sh` fetches it).

## How it works

- **Decoder** (`decode.cpp`, `isa_tables.inc`): GFX10 instruction formats; opcode names are
  generated from LLVM's AMDGPU disassembler (`tools/gen/gen_isa_tables.py`). It also accepts
  encodings the console compiler emits that stock LLVM rejects.
- **Containers and metadata** (`container.cpp`, `meta.cpp`): the precompiled code inside a shader
  container, its binding records, the root signature, signatures (ISG1/OSG1) and PSV0.
- **Translator** (`translate.cpp`): one HLSL invocation per console lane; control flow follows the
  console's wave-uniformly; vector writes are predicated on the lane's EXEC bit; lane masks are
  kept as the lane's bit and as the wave's ballot. Resources are rebuilt from the binding metadata
  and the title's root signature, so the PC pipeline keeps the title's root signature.
- **Wave64 on 32-lane PCs**: console wave64 code runs two console lanes per PC thread (registers
  and EXEC/VCC bits per copy, exact 64-bit masks), so wave operations keep wave64 semantics.
- **Typed resources**: HLSL needs a component type the console takes from the view's format at run
  time; it is inferred from the integer or float code that produces stored values and uses loaded
  ones.
- **Layer hook** (`src/gfx/native_shaders.cpp`): containers without DXIL are translated when a
  pipeline is created; results are cached on disk.

## Status

| Stage | State |
|---|---|
| Compute | All sample compute shaders translate; A/B against the DXIL path matches (differences at fused vs unfused multiply-add level); tier-1 kernels match exactly |
| Pixel, vertex | Samples' pixel and vertex shaders translate; SimpleTexture and SimpleLighting render bit-exact |
| Mesh | Translate (per-primitive outputs; groups up to 256 threads via the two-lane mapping); SimpleMeshlet renders on an RTX 5090 |
| Geometry | Shaders that copy input vertex fields (e.g. render-target slice selection) are translated from a one-lane run with tagged inputs; others not yet |
| Amplification, hull, domain | Not yet |
| Ray tracing | Inert: state objects load and indirect ray dispatches are skipped; ray queries (`image_bvh*_intersect_ray`) are translated as misses (every node test returns no children / no hit) |

The Matrix Awakens: all 365 distinct shaders translate.

Shaders that cannot be translated yet make their pipeline a placeholder (its work is skipped).

## Switches

| Variable | Effect |
|---|---|
| `XSL_NATIVE_SHADERS=0/1` | Never translate / translate every shader that has precompiled code (A/B testing) |
| `XSL_NATIVE_STAGES=CS,PS,...` | Translate only these stages |
| `XSL_SHADER_CACHE=<dir>` | Cache directory (default `%LOCALAPPDATA%\OpenScarlett\shadercache`) |
| `XSL_DUMP_SHADERS=<dir>` | Save each translated container and its HLSL |
| `XSL_PLACEHOLDER_PSO=0` | Fail pipelines with untranslated shaders instead of using a placeholder |
| `XSL_LOOP_GUARD=<n>` | Dispatcher steps after which an invocation stops (runaway loops; default 4096, 4M in shaders with barriers) |
| `XSL_SKIP_SHADERS=<keys>` | Treat these shaders as untranslatable (placeholder pipelines; for bisecting GPU hangs) |
| `XSL_FIXED_FPS=<n>` | Deterministic time (QueryPerformanceCounter advances 1/n s per present), for frame comparisons |
| `XSL_D3D12_DRED=1` | Device-removed breadcrumbs and page faults in the log |
| `XSL_SHADER_OVERRIDE=<dir>` | Compile `<dir>\<key>.hlsl` instead of the translation (hand-edited probes; never cached) |
| `XSL_TRACE_WRITERS=1` | Frame diagnostics: with `XSL_RT_STATS=<present>` (target statistics, R11G11B10 decoded), `XSL_TRACE_PSO=<keys>` (views, constants, sizes, pipeline state, indirect arguments), `XSL_PLANE_OPS=<present>` (every write to presented images), `XSL_TRACE_BUFFER=<bytes>`, `XSL_TRACE_TEX=<w>x<h>` (writers) |
| `XSL_DETILE_RECHECK=0` | Do not re-check uploaded console-tiled textures for later CPU writes |
| `XSL_RT_STATS=<present>`, `XSL_PLANE_OPS=<present>` | Without `XSL_TRACE_WRITERS=1`, writer tracing runs only for the 30 presents before that one |
| `XSL_GPU_PROFILE=<present>` / `slow` | GPU time per pipeline (draws, dispatches, indirect executes) of the frame before that present; `slow`: the frame after the first present over 250 ms |
| `XSL_INDIRECT_GUARD=0` | Execute indirect arguments as they are (by default dispatches over 65535 groups and draws of 2^28+ items become empty) |
| `XSL_DUMP_FAILED_PSO=<dir>` | Save the shaders of pipeline streams the PC runtime rejects |
| `XSL_MF_GPU=0` | Refuse Media Foundation a D3D12 device (no GPU video decoding) |
| `XSL_COMPUTE_AS_DIRECT=0` | Keep PC compute queues for console compute queues (default: direct queues) |
| `XSL_TRACE_VIDEO_PLANES=1` | Log placements of R8/R8G8 textures (video plane aliases) |

## Tests

- `tools/gpucode/compare_llvm.py`: decoder agreement with `llvm-objdump`, per instruction.
- `tools/gpucode/tier1 <dir>`: tier-1 kernels (`tests/gpucode/kernels`) compiled with the console
  compiler and translated, against the same kernels compiled for PC; `run.cmd` in the output runs
  them on a Windows PC with `dispatch_check` (exact comparison of everything they write).
- `tools/gpucode/asmtests <dir>`: hand-assembled ISA tests (`tests/gpucode/asm/*.s`, expected words
  in their headers) spliced into a template container; `run.cmd` checks them with
  `dispatch_check --expect`.
- `tests/gpucode/dispatch_check`: runs one translated compute shader in isolation (timing, hangs,
  comparison with a reference).
- `tools/gpucode/ab-samples`: renders each sample through the DXIL path and the translated path at
  a fixed frame and compares the images.
- `src/gpucode` builds natively (`cmake -S src/gpucode -B build-native`); `xsl-gpucode` disassembles
  containers, prints metadata and translates.

## Ray tracing, step R1: design

Console ray tracing is shader code: traversal walks a 4-wide BVH with raw global loads and
`image_bvh*_intersect_ray`, and a DXR pipeline is one compute program (scheduler, state
functions, per-ray stacks). R1 translates that code as it is. It needs, in order:

1. **Global memory.** Translated shaders read arbitrary title GPU addresses. Host facts measured on
   the test PC (Windows 10 21H2, RTX 5090): shader model 6.5 at most (no `ResourceDescriptorHeap`
   without the Agility SDK) and shader-visible heaps of at most 1,000,000 entries, all of them
   assigned to title heaps. The title writes only the start of its later heaps (highest slots
   written per 125k band: full, full, full, full, 505,288, none, 750,023, none), so the layer can
   reserve a range at the end of the global heap and stop with an error if the title ever writes
   into it.
   - Every buffer the title can address gets a raw SRV in that range: committed buffers (title
     address = PC address) and imported unified memory (title address = CPU address).
   - A sorted table {start, size, view index} lives in the range's first slot.
   - Root signatures get one appended descriptor table (SRVs in reserved register spaces); the
     layer binds it before draws and dispatches of pipelines whose shaders use global memory. Title
     root parameter indices are unchanged.
   - Global loads become a binary search in the table plus `ByteAddressBuffer.Load`; misses
     return 0.
2. **`image_bvh*_intersect_ray` in HLSL** over that memory (node id = address >> 3 | type;
   box16/box32 children sorted by entry distance, misses 0xFFFFFFFF; triangle results t/|det|,
   sign(det), v/|det|, w/|det|; descriptor base address bits 39:0 in 256-byte units), checked with
   hand-assembled ISA tests.
3. **Acceleration structures in the console format:** title builds (the console runtime's
   builder is part of D3D12.X, so the layer must provide one) and deserialized offline structures
   (`xg_xs.dll`'s BVH computer: DeserializeBVH / DecompressBVH).
4. **Ray-tracing pipelines:** `DeserializeStateObjectX` blobs carry the compiled program; translate it
   as a compute shader and run `DispatchRays` as a dispatch with the shader tables in global memory.

### Step 4 as implemented

Observed in the console runtime's code (the scheduler `main` and traversal sections of the
collections; offsets from The Matrix Awakens demo, layouts from disassembly):

- **Collections** (`DeserializeStateObjectX` blobs, `tools/gpucode/rtso`) hold sections: three to
  five `XDXR_TraceRay` traversal variants, then one section per export. Continuation segments
  (`<export>.ss_N`) end with `s_setpc_b64 s[30:31]`; pointers are code addresses >> 8. The dword 16
  bytes before an export's name record is 0 for ray-generation shaders and the payload size for hit
  and miss shaders. Hit groups are records {u32 length, UTF-16 name} x 4 (group, closest hit, any
  hit, intersection).
- **Identifiers are code addresses.** Titles ask collections (not pipelines) for identifiers and mix
  them in shader tables; pipelines of hit groups only are dispatched with ray-generation shaders of
  other collections. `gpucode::RtSpace` places every collection once (from `0x100000000`);
  identifiers are entry >> 8 (hit groups: closest hit, any hit, intersection in dwords 0..2). The
  traversal reads dword 0 of a hit record as the closest-hit pointer (0: none).
- **Runtime table** (scheduler argument `s[0:1]`): +0x0..0xc descriptor-table bases of the global
  root signature (SRV, CBV, sampler, UAV); +0x18 the scheduler's s12..s27: traversal-pointer table
  (one per lane, variant chosen by ray flags), width, height, depth, s17 (hit stride / 32 at bit 21,
  miss stride / 32 at bit 5), s18 (launch-index packing: y shift at bit 8, 32 - z shift in bits
  0..4), s19..s23 (passed to shaders as s8..s12: miss table, hit table, ray-generation record,
  callable table, all address >> 6), s24 stack bytes per lane; +0x90 ray-stack buffer descriptor.
  2-D launches use 8x4 lanes per group, 1-D 32.
- **Programs.** DXC time grows much faster than linearly with function size (a 49K-line program
  takes over 10 minutes even at `-Od`), so each collection is its own program (traversal + the
  collection + its scheduler). A scheduler call to code outside the program parks the active lanes
  (target and VGPRs v0..v63 to a lane-state buffer; target marked in the pipeline's discovery
  bitmap). A dispatch runs round 0 in the pipeline's ray-generation collection, then further rounds
  over the collections rays were seen to reach; a program resumes the lanes parked on its code just
  before the scheduler's loop (the scheduler's prologue is re-run; it is deterministic per wave).
- **Cache.** Programs are cached by `rt_cache_key` (included code, scheduler, parking settings,
  translator version, compiler option). `xsl-gpucode rtcache <dir> gpucode-35-Od <blobs...>` writes
  every program's HLSL from blobs dumped with `XSL_DUMP_STATE_OBJECTS` (in deserialization order);
  compiling those to `rt_<key>.dxil` in the cache directory avoids compiling in game.

| Switch | Effect |
|---|---|
| `XSL_RT_PIPELINES=0` | Leave ray-tracing pipelines inert (no translation, ray dispatches skipped) |
| `XSL_RT_ROUNDS=<n>` | Program rounds per ray dispatch (default 6) |
| `XSL_RT_LANES=<n>` | Capacity of the parked-lane state (default 1M lanes, 264 bytes each) |
| `XSL_RT_DXC_OPT=Od/O1/O2/O3` | Optimization level of ray-tracing programs (default Od) |
| `XSL_RT_BUILD_THREADS=<n>` | Worker threads compiling ray-tracing programs (default a quarter of the processors, 1..4) |
| `XSL_DUMP_STATE_OBJECTS=<dir>` | Save deserialized collections (in order) and state-object descriptions |
