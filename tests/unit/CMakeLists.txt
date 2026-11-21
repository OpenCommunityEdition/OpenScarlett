# SPDX-License-Identifier: MIT
add_executable(test_dxbc_checksum test_dxbc_checksum.cpp ${CMAKE_SOURCE_DIR}/src/gfx/dxbc.cpp ${CMAKE_SOURCE_DIR}/src/gfx/native_shaders.cpp)
target_link_libraries(test_dxbc_checksum PRIVATE xsl_common xsl_gpucode)
target_include_directories(test_dxbc_checksum PRIVATE "${GXDK_ROOT}/xbox/include/devpctools/gen9" "${CMAKE_SOURCE_DIR}/src/gfx")
add_executable(pso_check ${CMAKE_SOURCE_DIR}/tests/gpucode/pso_check.cpp)
target_link_libraries(pso_check PRIVATE d3d12)
add_executable(dispatch_check ${CMAKE_SOURCE_DIR}/tests/gpucode/dispatch_check.cpp)
target_link_libraries(dispatch_check PRIVATE d3d12 xsl_gpucode)
target_include_directories(dispatch_check PRIVATE ${CMAKE_SOURCE_DIR}/src/gpucode)
