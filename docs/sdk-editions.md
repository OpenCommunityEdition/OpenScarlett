# Older GDK editions

A title calls the console D3D12 vtable layout and the Gaming Runtime interface GUIDs of the GDK
it was built with (its `.xbld` section says which: `_xbld_edition_sdktype=GXDK,220300`). On a
console the title ships the matching `d3d12_xs.dll` in its Game OS image; the layer instead
implements the pinned SDK's layout and remaps it per title:

- **D3D12.X:** `src/gfx/xs_abi.cpp` hands each object its function table rearranged into the
  title's edition (exact edition, else the oldest known one after it). Layouts come from every
  edition's `d3d12_xs.h` (`tools/gen/gen_abi_layouts.py`).
- **Gaming Runtime:** the dispatch table holds the thunks of every edition's `xgameruntime.lib`
  (`tools/gen/gen_runtime_dispatch.py`, newest first), so each edition's interface GUID and slot
  reaches the implementation of the same name. Unknown interfaces of a known class fall back to
  the class's base interface.

## Collecting the files

Both files come from GDK installs the user already has (never committed). Point
`tools/sdk/collect_editions.py` at installed or extracted GDK trees, or at ZIPs of them; it copies
only those two files per edition into an output directory with a manifest:

    tools/sdk/collect_editions.py /path/to/editions "/path/to/Microsoft GDK" other-gdk.zip ...
    cmake -S . -B build -DXSL_SDK_EDITIONS_DIR=/path/to/editions

`XSL_ABI_HEADERS` / `XSL_RT_OLD_LIBS` still accept explicit files. Titles whose edition is missing
use the oldest known layout after their edition (D3D12.X) and the class base-interface fallback
(runtime); collecting the matching edition gives exact tables.
