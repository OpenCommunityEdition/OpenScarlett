# SPDX-License-Identifier: MIT
# libHttpClient's XTaskQueue/XAsync implementation (Microsoft, MIT; see LICENSE, UPSTREAM).
add_library(xsl_taskqueue STATIC
  Source/Task/AsyncLib.cpp Source/Task/TaskQueue.cpp
  Source/Task/ThreadPool_win32.cpp Source/Task/WaitTimer_win32.cpp)
target_include_directories(xsl_taskqueue PUBLIC Include PRIVATE Source/Task)
target_compile_definitions(xsl_taskqueue PUBLIC HC_PLATFORM=HC_PLATFORM_WIN32 PRIVATE UNICODE _UNICODE)
target_compile_options(xsl_taskqueue PRIVATE -Wno-everything)
