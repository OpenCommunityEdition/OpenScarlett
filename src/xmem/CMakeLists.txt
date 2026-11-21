# SPDX-License-Identifier: AGPL-3.0-or-later
# xmem.dll replacement.
add_compile_definitions(UNICODE _UNICODE)
add_library(xmem SHARED xmem.cpp xmem.def)
target_include_directories(xmem PRIVATE "${XSL_CASE_DIR}" "${GXDK_ROOT}/xbox/include")
target_compile_options(xmem PRIVATE -Wno-everything)
target_link_libraries(xmem PRIVATE xsl_common onecore)
set_target_properties(xmem PROPERTIES OUTPUT_NAME xmem PREFIX "")
