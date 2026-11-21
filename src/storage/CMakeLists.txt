# SPDX-License-Identifier: AGPL-3.0-or-later
# XCRDAPI.dll: console DirectStorage (DStorageGetFactory) over Win32 file I/O and zlib.
add_compile_definitions(UNICODE _UNICODE)
add_library(XCRDAPI SHARED xcrdapi.cpp XCRDAPI.def)
target_include_directories(XCRDAPI PRIVATE "${GXDK_ROOT}/xbox/include/gen9" "${GXDK_ROOT}/xbox/include")
target_compile_options(XCRDAPI PRIVATE -Wno-everything)
target_link_libraries(XCRDAPI PRIVATE xsl_zlib xsl_common)
set_target_properties(XCRDAPI PROPERTIES OUTPUT_NAME XCRDAPI PREFIX "")
