# SPDX-License-Identifier: AGPL-3.0-or-later
# MFPlat.dll: the system Media Foundation platform DLL plus the console-only exports.
set(_mf_gen_cpp "${XSL_GEN_DIR}/mfplat_forward.cpp")
set(_mf_gen_def "${XSL_GEN_DIR}/mfplat_forward.def")
add_custom_command(
  OUTPUT "${_mf_gen_cpp}" "${_mf_gen_def}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/gen_forwarder.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/mfplat_exports.txt" "${_mf_gen_cpp}" "${_mf_gen_def}" MFPlat
          --own MFResetDXGIDeviceManagerX
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen/gen_forwarder.py" "${CMAKE_CURRENT_SOURCE_DIR}/mfplat_exports.txt"
  COMMENT "Generating MFPlat.dll forwarders")
add_library(MFPlat SHARED mfplat.cpp "${_mf_gen_cpp}" "${_mf_gen_def}")
target_link_libraries(MFPlat PRIVATE xsl_common)
set_target_properties(MFPlat PROPERTIES OUTPUT_NAME MFPlat)
