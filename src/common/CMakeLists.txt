# SPDX-License-Identifier: AGPL-3.0-or-later
add_compile_definitions(UNICODE _UNICODE)
add_library(xsl_common STATIC log.cpp)
target_include_directories(xsl_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
