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
- **Layer hook** (`src/gfx/native_shaders.cpp`): containers without DXIL are translated when a
  pipeline is created; results are cached on disk.

## Status

| Stage | State |
|---|---|
| Compute | All sample compute shaders translate; A/B against the DXIL path matches (differences at fused vs unfused multiply-add level) |
| Pixel, vertex | Samples' pixel and vertex shaders translate; SimpleTexture and SimpleLighting render bit-exact |
| Mesh | Translate; SimpleMeshlet renders on an RTX 5090. Groups larger than 128 threads are not supported yet |
| Amplification, hull, domain, geometry | Not yet |
| Ray tracing | Inert (state objects load, dispatches do nothing) |

Shaders that cannot be translated yet make their pipeline a placeholder (its work is skipped).

## Switches

| Variable | Effect |
|---|---|
| `XSL_NATIVE_SHADERS=0/1` | Never translate / translate every shader that has precompiled code (A/B testing) |
| `XSL_NATIVE_STAGES=CS,PS,...` | Translate only these stages |
| `XSL_SHADER_CACHE=<dir>` | Cache directory (default `%LOCALAPPDATA%\OpenScarlett\shadercache`) |
| `XSL_DUMP_SHADERS=<dir>` | Save each translated container and its HLSL |
| `XSL_PLACEHOLDER_PSO=0` | Fail pipelines with untranslated shaders instead of using a placeholder |
| `XSL_FIXED_FPS=<n>` | Deterministic time (QueryPerformanceCounter advances 1/n s per present), for frame comparisons |
| `XSL_D3D12_DRED=1` | Device-removed breadcrumbs and page faults in the log |

## Tests

- `tools/gpucode/compare_llvm.py`: decoder agreement with `llvm-objdump`, per instruction.
- `tools/gpucode/ab-samples`: renders each sample through the DXIL path and the translated path at
  a fixed frame and compares the images.
- `src/gpucode` builds natively (`cmake -S src/gpucode -B build-native`); `xsl-gpucode` disassembles
  containers, prints metadata and translates.
