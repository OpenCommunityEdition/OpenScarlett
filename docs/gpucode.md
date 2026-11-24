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
| `XSL_PSO_ASYNC_MS=<ms>`, `XSL_PSO_THREADS=<n>` | Compute pipelines that take longer than this to create (default 300 ms) finish on a pool of n threads (default 3); their dispatches are skipped until then |
| `XSL_PSO_CACHE=0` | Do not keep the driver's compiled pipelines (`pso_*.bin`, `rtpso_*.bin`) in the cache directory |
| `XSL_GM_WATCH=<hex address>` | Log registrations, releases and table rows of the global-memory buffer holding that address |
| `XSL_HANG_WATCH=1` | When a fence wait stalls: queue state and the stacks of the title's threads |

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
  other collections. `gpucode::RtSpace` gives every collection's code and every distinct traversal
  section a 1 MiB slot chosen by a hash of its contents (from `0x100000000`; titles load
  collections on several threads, so placement must not depend on order); identifiers are entry >> 8 (hit groups: closest hit, any hit, intersection in dwords 0..2). The
  traversal reads dword 0 of a hit record as the closest-hit pointer (0: none).
- **Runtime table** (scheduler argument `s[0:1]`): +0x0..0xc descriptor-table bases of the global
  root signature (SRV, CBV, sampler, UAV); +0x18 the scheduler's s12..s27: traversal-pointer table
  (one per lane, variant chosen by ray flags), width, height, depth, s17 (hit stride / 32 at bit 21,
  miss stride / 32 at bit 5), s18 (launch-index packing: y shift at bit 8, 32 - z shift in bits
  0..4), s19..s23 (passed to shaders as s8..s12: miss table, hit table, ray-generation record,
  callable table, all address >> 6), s24 stack bytes per lane; +0x90 ray-stack buffer descriptor.
  2-D launches use 8x4 lanes per group, 1-D 32.
- **Wave size.** The code is wave32 but uses 64-bit forms on the EXEC pair (`s_or_saveexec_b64 sN,
  -1` around sections that run every lane). Any `saveexec_b32`/`wrexec_b32` decides wave32; read as
  wave64, every compare result also wrote the following register (in the scheduler that was the
  ray-generation pointer it had just loaded, so those pipelines traced nothing). In wave32 code a
  64-bit scalar logic or EXEC instruction writes both registers, and EXEC/VCC zero tests look at the
  low register only.
- **Ray stack.** The scheduler converts byte offsets with `>> 5` and sizes a wave's stack as
  `0x200 + 32 * s24`: the stack descriptor is a scratch-style buffer with ADD_TID_ENABLE (dword 3
  bit 23), i.e. swizzled per lane: dword offset `o` of lane `t` is at `o * 32 + t * 4` after the
  scalar offset. Functions save registers to frames at the end of the wave's stack (`s32`) with the
  same addressing (offsets up to 656 bytes seen). `TranslateOptions::raw_tid_buffers` applies that
  to single-dword accesses through run-time descriptors with the bit set; a wave's slot is
  `rt_wave_stack_bytes()` and there are `kRtStackSlots` of them (lanes parked between rounds keep
  their stack, so slots must outnumber the waves of a dispatch).
- **Traversal per program.** A shader asks for a trace by returning pointer 42; the scheduler picks
  a variant (0..2 from the ray flags) and reads the traversal pointer from that *lane* of a VGPR it
  loaded from the runtime table. The table names one traversal for the pipeline, but a program
  holds only its collection's traversal sections (two sets exist in the demo), so at the resume
  point every program replaces that VGPR with its own entries (`RtLinked::trav_vgpr`); otherwise
  lanes parked on traversal code no program of the round contained.
- **Programs.** DXC time grows much faster than linearly with function size (a 49K-line program
  takes over 10 minutes even at `-Od`), so each collection is its own program (traversal + the
  collection + its scheduler). A scheduler call to code outside the program parks the active lanes
  (target and VGPRs v0..v63 to a lane-state buffer; the target's slot marked in the pipeline's
  discovery bitmap). A program is several code segments at their own addresses
  (`TranslateOptions::raw_segments`). A dispatch runs round 0 in the pipeline's ray-generation collection, then further rounds
  over the collections rays were seen to reach; a program resumes the lanes parked on its code just
  before the scheduler's loop (the scheduler's prologue is re-run; it is deterministic per wave).
- **Cache.** Programs are cached by `rt_cache_key` (included code, scheduler, parking settings,
  translator version, compiler option). `xsl-gpucode rtcache <dir> gpucode-35-O3 <blobs...>` writes
  every program's HLSL from blobs dumped with `XSL_DUMP_STATE_OBJECTS` (in deserialization order);
  compiling those to `rt_<key>.dxil` in the cache directory avoids compiling in game.

| Switch | Effect |
|---|---|
| `XSL_RT_PIPELINES=0` | Leave ray-tracing pipelines inert (no translation, ray dispatches skipped) |
| `XSL_RT_ROUNDS=<n>` | Program rounds per ray dispatch (default 12; a ray needs one round per change of collection) |
| `XSL_RT_LANES=<n>` | Capacity of the parked-lane state (default 1M lanes, 264 bytes each) |
| `XSL_RT_DXC_OPT=Od/O1/O2/O3` | Optimization level of ray-tracing programs (default O3; part of the cache key) |
| `XSL_RT_BUILD_THREADS=<n>` | Worker threads compiling ray-tracing programs (default 1, at idle priority) |
| `XSL_DUMP_STATE_OBJECTS=<dir>` | Save deserialized collections (in order) and state-object descriptions |
| `XSL_RT_DEBUG=1` | Programs with counters (other cache keys): lanes, parked/resumed lanes and targets, BVH tests, global-memory misses, the scheduler's view of the runtime table; logged per pipeline with the frame rate |
| `XSL_RT_GUARD=<n>` | Dispatcher steps per invocation of a ray-tracing program (default 65536; other cache keys) |
| `XSL_RT_WARM=1` | Create every cached program's pipeline at start (fills the driver-blob cache `rtpso_*.bin`) |

Dispatches of a pipeline are left out while its start program or the program of a collection its
rays were seen to reach is still being built (rays that cannot finish leave their outputs
unwritten); the frame-rate log counts them by reason. Cached programs load on a thread of their
own, ahead of compiles. A pipeline made without collections (configuration only; the title
dispatches it with identifiers of collections it loaded on its own) starts through any ready
collection's scheduler, and one dispatched as a deserialized state object is a pipeline of that
collection.

Open: one traversal variant set calls any-hit and intersection shaders directly (`s_swappc`
inside the traversal, pointer from the hit record). When that shader is in another collection the
jump target is not in the program and the wave ends; this is the suspected cause of the objects
that are still black. The traversal presets the shader's answer to "accept", so letting such calls
fall through would count the hit as opaque (not tried yet).

The GPU driver compiles one pipeline at a time per process and needs 15 s to several minutes for
each of these programs. `xsl-rtwarm <shard> <shards>` (`tools/rtwarm`, `ninja xsl-rtwarm`) does
that outside the game for the `rt_*.dxil` files in the cache directory; one process per shard
(e.g. 12) uses the whole machine. `RT_STATS=1 xsl-gpucode rtcache ...` writes the `XSL_RT_DEBUG`
variants.

### Filling the cache before a run

`xsl-shaderwarm <folder>... [--jobs N] [--check] [--report FILE]` (`tools/shaderwarm`,
`ninja xsl-shaderwarm`) translates and compiles every console shader container found in a folder
into the cache, with the code and the cache key the layer uses when a pipeline is created. What is
in the cache already is skipped, so a stopped run continues, and after a translator update the
cache is refilled here instead of at first use in the title. `--check` only counts what the cache
has and lacks; `--report` lists the shaders that did not translate with the reason, and the tool
ends with those reasons counted, which shows what the translator still lacks for a set of shaders.

A shader that describes its own root signature is translated against that one; a shader that needs
its pipeline's root signature cannot be prepared this way. The translation follows the GPU
(wave sizes, shader model), so run it on the machine the title will run on. The tool looks for the
shader compiler and the D3D12 runtime next to itself; `tools/xsl-warm <folder> [--cache DIR]`
(Windows) puts them there and runs it. Where the containers come from is up to you: files dumped
from a run, or extracted from your own copy of a title with a tool that knows its archives.
