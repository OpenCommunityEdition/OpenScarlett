# SPDX-License-Identifier: MIT
add_executable(test_dxbc_checksum test_dxbc_checksum.cpp ${CMAKE_SOURCE_DIR}/src/gfx/dxbc.cpp)
target_link_libraries(test_dxbc_checksum PRIVATE xsl_common)
target_include_directories(test_dxbc_checksum PRIVATE "${GXDK_ROOT}/xbox/include/devpctools/gen9")
