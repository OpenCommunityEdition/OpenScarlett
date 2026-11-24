# SPDX-License-Identifier: AGPL-3.0-or-later
add_compile_definitions(UNICODE _UNICODE)
# d3d12_xs.dll: console Direct3D 12.X over PC Direct3D 12.

# PC half: Windows SDK headers only.
add_library(xsl_gfx_pc STATIC pc_backend.cpp dxbc.cpp native_shaders.cpp)
target_link_libraries(xsl_gfx_pc PUBLIC xsl_common d3d12 dxgi xsl_gpucode)
# dxcapi_xs.h (IDxcPdbUtils) for expanding slim shader PDBs; the DLL itself is loaded at run time.
target_include_directories(xsl_gfx_pc PRIVATE "${GXDK_ROOT}/xbox/include/devpctools/gen9")

# Xbox half: GDKX headers only (never in the same translation unit as d3d12.h).
add_library(d3d12_xs SHARED fixed_time.cpp xs_objects.cpp xs_pagemap.cpp xs_descriptors.cpp xs_device.cpp xs_cbv.cpp xs_detile.cpp xs_unified.cpp xs_exports.cpp xs_abi.cpp rect_gs.cpp d3d12_xs.def)
target_compile_definitions(d3d12_xs PRIVATE XSL_PINNED_GXDK_EDITION=${XSL_PINNED_GXDK_EDITION})
add_dependencies(d3d12_xs xsl_gen)
target_include_directories(d3d12_xs PRIVATE ${XSL_GEN_DIR} "${GXDK_ROOT}/xbox/include/gen9" "${GXDK_ROOT}/xbox/include")
target_compile_options(d3d12_xs PRIVATE -Wno-everything)
target_link_libraries(d3d12_xs PRIVATE xsl_gfx_pc xsl_platform xsl_common)
set_target_properties(d3d12_xs PROPERTIES OUTPUT_NAME d3d12_xs PREFIX "")

# Offline warm-up of the driver's pipeline cache for translated ray-tracing programs (tools/rtwarm).
add_executable(xsl-rtwarm EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tools/rtwarm/main.cpp)
target_link_libraries(xsl-rtwarm PRIVATE xsl_gfx_pc xsl_platform xsl_common d3d12 dxgi dxguid)
