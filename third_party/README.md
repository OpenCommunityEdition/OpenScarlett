# Third-party code

| Path | Upstream | Licence |
|---|---|---|
| `zlib/` | zlib 1.3.1 (vendored, unmodified apart from the build file) | zlib (see `zlib/README`) |
| `libHttpClient-task/` | The task-queue part of Microsoft's open-source libHttpClient (`UPSTREAM` names the commit) | MIT (`libHttpClient-task/LICENSE.md`) |
| `dxil-spirv-*.patch` | Patches to dxil-spirv, applied by `tools/build-vkd3d-proton.sh` | MIT, as upstream dxil-spirv (`LICENSES/MIT.txt`) |
| `vkd3d-proton-openscarlett.patch` | Patch to vkd3d-proton, applied by `tools/build-vkd3d-proton.sh` | LGPL-2.1-or-later, as upstream vkd3d-proton (`LICENSES/LGPL-2.1-or-later.txt`) |

The patches keep their upstream licences so they can be contributed back.
