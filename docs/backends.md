# Direct3D 12 back ends

The layer turns console D3D12.X calls into PC Direct3D 12 calls. Which PC D3D12 implementation
runs them is chosen with `tools/xsl-run --backend` or `XSL_BACKEND`:

| Back end | Platform | What runs the D3D12 calls | Files placed next to the title |
|---|---|---|---|
| `native` (Windows default) | Windows 10/11 | the system's Direct3D 12 and DXGI | none |
| `vkd3d-proton-xsl` (Linux default) | Linux, Wine | vkd3d-proton 3.1 rebuilt with the layer's dxil-spirv patch | `build/deps/vkd3d-proton-xsl/x64/{d3d12,d3d12core}.dll`, `build/deps/dxvk/dxvk-3.1.1/x64/dxgi.dll` |
| `vkd3d-proton` | Linux, Wine | stock vkd3d-proton 3.1 (Xbox DXIL features missing) | `build/deps/vkd3d-proton/vkd3d-proton-3.1/x64/...`, DXVK 3.1.1 `dxgi.dll` |
| `wine` | Linux, Wine | Wine's builtin d3d12 (upstream vkd3d) | none |
| `null` | either | no GPU work (`XSL_NULL_GRAPHICS=1`), for running a frame loop | as `vkd3d-proton-xsl` |

## Getting the files

- `vkd3d-proton-xsl`: `tools/build-vkd3d-proton.sh` applies `third_party/vkd3d-proton-*.patch`
  to vkd3d-proton 3.1 and installs to `build/deps/vkd3d-proton-xsl/x64`.
- `vkd3d-proton` and DXVK: the release archives (vkd3d-proton 3.1, DXVK 3.1.1) unpacked under
  `build/deps/vkd3d-proton/` and `build/deps/dxvk/`.
- The PC shader compiler used for console machine-code shaders (`dxcompiler.dll`, `dxil.dll`):
  `tools/get-dxc.sh` (any back end).

`xsl-run` stops with `missing … (see docs/backends.md)` when a file of the chosen back end is
absent.
