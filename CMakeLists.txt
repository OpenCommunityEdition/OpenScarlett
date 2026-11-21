# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(OpenScarlett LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: layer DLLs carry no VC runtime dependency
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The licensed GDKX is a build-time dependency only (hygiene rules 2 and 3).
set(GXDK_ROOT "${CMAKE_SOURCE_DIR}/sdk" CACHE PATH "Root of the GDKX edition (…/Microsoft GDK/260405)")
if(NOT EXISTS "${GXDK_ROOT}/xbox/include/gen9/d3d12_xs.h")
  message(FATAL_ERROR "GXDK_ROOT does not point at a GDKX edition: ${GXDK_ROOT}")
endif()
set(XSL_SAMPLES_ROOT "${CMAKE_SOURCE_DIR}/../xbox-emu/gdk-samples/Xbox-GDK-Samples" CACHE PATH "Microsoft Xbox-GDK-Samples checkout")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Debug info for crash symbolization (PDBs stay in the build tree; *.pdb is gitignored).
add_compile_options(/Z7)
add_link_options(/DEBUG)
add_compile_options(/W3 /EHsc /Zc:__cplusplus /utf-8
  -Wno-microsoft-cast -Wno-unused-parameter -Wno-nonportable-include-path
  -Wno-pragma-pack -Wno-ignored-attributes -Wno-unknown-pragmas)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

# Linux is case-sensitive; the SDK and samples spell some include names differently from the files.
set(XSL_CASE_DIR "${CMAKE_BINARY_DIR}/sample-case")
execute_process(COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/samples/case_links.py" "${XSL_CASE_DIR}"
  --headers "${GXDK_ROOT}/xbox/include/gen9" "${GXDK_ROOT}/xbox/include"
            "${XSL_SAMPLES_ROOT}/Kits/ATGTK" "${XSL_SAMPLES_ROOT}/Kits/DirectXTK12/Inc"
  --sources "${XSL_SAMPLES_ROOT}/Kits" "${XSL_SAMPLES_ROOT}/Samples" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests")

# Generated, SDK-derived code lives only in the build tree.
set(XSL_GEN_DIR "${CMAKE_BINARY_DIR}/gen")
add_custom_command(
  OUTPUT "${XSL_GEN_DIR}/xs_tables.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${XSL_GEN_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/gen_function_tables.py"
          "${GXDK_ROOT}/xbox/include/gen9/d3d12_xs.h" "${XSL_GEN_DIR}/xs_tables.h"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen/gen_function_tables.py" "${GXDK_ROOT}/xbox/include/gen9/d3d12_xs.h"
  COMMENT "Generating function-table stubs from d3d12_xs.h")
# Older GDK editions' console D3D12 layouts (src/gfx/xs_abi.cpp): EDITION=path/to/d3d12_xs.h
# entries from the user's own older GDKX installs, e.g. 221000=/path/2022.10/d3d12_xs.h.
set(XSL_ABI_HEADERS "" CACHE STRING "EDITION=d3d12_xs.h list of older GDKX headers for older titles")
set(XSL_PINNED_GXDK_EDITION 260405 CACHE STRING "GXDK edition of GXDK_ROOT (its .xbld sdktype)")
# Each GDK edition's d3d12_xs.h and xgameruntime.lib, as collected by tools/sdk/collect_editions.py
# (DIR/manifest.tsv). Used by both the D3D12 ABI layouts and the runtime dispatch.
set(XSL_SDK_EDITIONS_DIR "" CACHE PATH "tools/sdk/collect_editions.py output directory")
set(_editions_args)
set(_editions_dep)
if(XSL_SDK_EDITIONS_DIR)
  set(_editions_args --editions-dir "${XSL_SDK_EDITIONS_DIR}")
  set(_editions_dep "${XSL_SDK_EDITIONS_DIR}/manifest.tsv")
endif()
set(_abi_deps)
foreach(_e ${XSL_ABI_HEADERS})
  string(REGEX REPLACE "^[0-9]+=" "" _p "${_e}")
  list(APPEND _abi_deps "${_p}")
endforeach()
add_custom_command(
  OUTPUT "${XSL_GEN_DIR}/xs_abi_layouts.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${XSL_GEN_DIR}"
  COMMAND ${CMAKE_COMMAND} -E env XSL_PINNED_GXDK_EDITION=${XSL_PINNED_GXDK_EDITION}
          ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/gen_abi_layouts.py"
          "${GXDK_ROOT}/xbox/include/gen9/d3d12_xs.h" "${XSL_GEN_DIR}/xs_abi_layouts.h" ${XSL_ABI_HEADERS} ${_editions_args}
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen/gen_abi_layouts.py" "${GXDK_ROOT}/xbox/include/gen9/d3d12_xs.h" ${_abi_deps} ${_editions_dep}
  COMMENT "Generating older console D3D12 ABI layouts")
add_custom_target(xsl_gen DEPENDS "${XSL_GEN_DIR}/xs_tables.h" "${XSL_GEN_DIR}/xs_abi_layouts.h")

add_subdirectory(src/common)
add_subdirectory(src/platform)
add_subdirectory(src/gfx)
add_subdirectory(third_party/libHttpClient-task)
add_subdirectory(src/runtime)
add_subdirectory(src/input)
add_subdirectory(src/xmem)
add_subdirectory(src/audio)
add_subdirectory(third_party/zlib)
add_subdirectory(src/storage)

option(XSL_BUILD_SAMPLES "Build Microsoft samples as Series X|S executables" ON)
if(XSL_BUILD_SAMPLES)
  add_subdirectory(tests/samples)
endif()
add_subdirectory(tests/unit)
