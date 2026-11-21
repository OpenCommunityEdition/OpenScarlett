# SPDX-License-Identifier: MIT
# Cross-compile MSVC-ABI PE binaries on Linux with clang-cl, lld-link and an
# xwin-provided Windows SDK and CRT.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

if(NOT XWIN_ROOT)
  set(XWIN_ROOT "$ENV{HOME}/.xwin")
endif()
set(XWIN_ROOT "${XWIN_ROOT}" CACHE PATH "xwin splat directory")

set(_xwin_inc
  "/imsvc${XWIN_ROOT}/crt/include"
  "/imsvc${XWIN_ROOT}/sdk/include/ucrt"
  "/imsvc${XWIN_ROOT}/sdk/include/um"
  "/imsvc${XWIN_ROOT}/sdk/include/shared"
  "/imsvc${XWIN_ROOT}/sdk/include/winrt")
string(JOIN " " _xwin_inc_str ${_xwin_inc})
set(CMAKE_C_FLAGS_INIT "${_xwin_inc_str}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_inc_str}")

set(_xwin_lib
  "/libpath:${XWIN_ROOT}/crt/lib/x86_64"
  "/libpath:${XWIN_ROOT}/sdk/lib/ucrt/x86_64"
  "/libpath:${XWIN_ROOT}/sdk/lib/um/x86_64")
string(JOIN " " _xwin_lib_str ${_xwin_lib})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_lib_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib_str}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib_str}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
