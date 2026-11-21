# SPDX-License-Identifier: AGPL-3.0-or-later
# xgameruntime.dll: the Gaming Runtime as the title's static xgameruntime.lib finds it.
add_compile_definitions(UNICODE _UNICODE)
# xgameruntime.lib of older GDKX installs, for titles built with them (their interface GUIDs).
set(XSL_RT_OLD_LIBS "" CACHE STRING "older SDKs' xgameruntime.lib files (list)")
add_custom_command(
  OUTPUT "${XSL_GEN_DIR}/rt_dispatch.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${XSL_GEN_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/gen_runtime_dispatch.py"
          "${GXDK_ROOT}/xbox/lib/x64/xgameruntime.lib" llvm-objdump "${XSL_GEN_DIR}/rt_dispatch.h" ${XSL_RT_OLD_LIBS} ${_editions_args}
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen/gen_runtime_dispatch.py" "${GXDK_ROOT}/xbox/lib/x64/xgameruntime.lib" ${XSL_RT_OLD_LIBS} ${_editions_dep}
  COMMENT "Generating Gaming Runtime dispatch from xgameruntime.lib")
add_custom_target(xsl_rt_gen DEPENDS "${XSL_GEN_DIR}/rt_dispatch.h")

# XTaskQueue/XAsync from libHttpClient: its headers must not meet the GDK's in one TU.
add_library(xsl_rt_task OBJECT rt_task.cpp)
target_link_libraries(xsl_rt_task PRIVATE xsl_taskqueue xsl_common)
target_compile_options(xsl_rt_task PRIVATE -Wno-everything)

add_library(xsl_rt_gdk OBJECT rt_system.cpp rt_storage.cpp rt_user.cpp rt_events.cpp rt_gamesave.cpp)
target_include_directories(xsl_rt_gdk PRIVATE "${GXDK_ROOT}/xbox/include")
target_link_libraries(xsl_rt_gdk PRIVATE xsl_common)
target_compile_options(xsl_rt_gdk PRIVATE -Wno-everything)

add_library(xgameruntime SHARED runtime.cpp runtime_classes.cpp xgameruntime.def
  $<TARGET_OBJECTS:xsl_rt_task> $<TARGET_OBJECTS:xsl_rt_gdk>)
add_dependencies(xgameruntime xsl_rt_gen)
target_include_directories(xgameruntime PRIVATE ${XSL_GEN_DIR})
target_link_libraries(xgameruntime PRIVATE xsl_taskqueue xsl_common)
set_target_properties(xgameruntime PROPERTIES OUTPUT_NAME xgameruntime PREFIX "")
