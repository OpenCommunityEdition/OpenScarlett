# SPDX-License-Identifier: AGPL-3.0-or-later
# GameInput.dll replacement.
add_compile_definitions(UNICODE _UNICODE)
add_library(GameInput SHARED gameinput.cpp GameInput.def)
target_include_directories(GameInput PRIVATE "${GXDK_ROOT}/xbox/include")
target_compile_options(GameInput PRIVATE -Wno-everything)
target_link_libraries(GameInput PRIVATE xsl_common setupapi hid)
set_target_properties(GameInput PROPERTIES OUTPUT_NAME GameInput PREFIX "")
