# SPDX-License-Identifier: AGPL-3.0-or-later
# Platform surface: Win32 entries that need console behaviour, interposed by
# patching the title's import table before its entry point runs.
add_compile_definitions(UNICODE _UNICODE)
add_library(xsl_platform STATIC iat.cpp lifecycle.cpp windowing.cpp user_patches.cpp host_windows.cpp net_trace.cpp)
target_include_directories(xsl_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xsl_platform PUBLIC xsl_common ws2_32)
