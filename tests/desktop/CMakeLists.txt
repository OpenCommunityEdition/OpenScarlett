# SPDX-License-Identifier: MIT
#
# Reference builds: the PC counterparts of samples, built for desktop Windows with
# the GDK's desktop headers/libraries and the PC shader compiler (DXC release, build/deps/dxc-pc,
# see tools/get-dxc.sh). They run on real PC Direct3D 12 (vkd3d-proton under Wine) and give the
# reference images the layer's output is compared with (tools/compare-reference). Outputs stay in
# build/desktop.
set(XSL_PC_DXC "${CMAKE_BINARY_DIR}/deps/dxc-pc/bin/x64/dxc.exe")
if(NOT EXISTS "${XSL_PC_DXC}")
  message(STATUS "reference builds skipped: no PC dxc at ${XSL_PC_DXC} (tools/get-dxc.sh)")
  return()
endif()
set(DESK_SHADERS "${CMAKE_BINARY_DIR}/dxtk12-shaders-desktop")
add_custom_command(OUTPUT "${DESK_SHADERS}/.stamp"
  COMMAND ${Python3_EXECUTABLE} "${XSL_TOOLS}/dxtk_shaders.py" "${DXTK}/Src/Shaders" "${XSL_PC_DXC}" "${DESK_SHADERS}" "${XSL_TOOL_PREFIX}" desktop
  COMMAND ${CMAKE_COMMAND} -E touch "${DESK_SHADERS}/.stamp"
  DEPENDS "${XSL_TOOLS}/dxtk_shaders.py" ${DXTK_FX}
  COMMENT "Compiling DirectXTK12 shaders for desktop")
add_custom_target(dxtk12_desktop_shaders DEPENDS "${DESK_SHADERS}/.stamp")

function(xsl_desktop_settings tgt)
  set_target_properties(${tgt} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  target_compile_definitions(${tgt} PRIVATE _GAMING_DESKTOP WIN32 _WINDOWS UNICODE _UNICODE NDEBUG)
  target_compile_options(${tgt} PRIVATE /Zc:char8_t- /O2 -Wno-everything)
  target_include_directories(${tgt} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${XSL_CASE_DIR}" "${GXDK_ROOT}/windows/include")
endfunction()

set(DESK_DXTK_SRC ${DXTK_SRC})
list(FILTER DESK_DXTK_SRC EXCLUDE REGEX "Xbox[^/]*\\.cpp$")  # Xbox-only modules
add_library(dtk12_desktop STATIC ${DESK_DXTK_SRC})
add_dependencies(dtk12_desktop dxtk12_desktop_shaders)
xsl_desktop_settings(dtk12_desktop)
target_include_directories(dtk12_desktop PRIVATE "${DXTK}/Inc" "${DXTK}/Src" "${DESK_SHADERS}")

# xsl_add_desktop(<name> <vcxproj> <platform>)
function(xsl_add_desktop name vcxproj platform)
  set(meta "${CMAKE_CURRENT_BINARY_DIR}/${name}.vcx.cmake")
  execute_process(COMMAND ${Python3_EXECUTABLE} "${XSL_TOOLS}/vcxproj2cmake.py" "${vcxproj}" "${meta}" "${platform}" RESULT_VARIABLE rc)
  if(rc)
    message(WARNING "skipping reference ${name}")
    return()
  endif()
  include("${meta}")
  set(outdir "${CMAKE_BINARY_DIR}/desktop/${name}")
  add_executable(${name} WIN32 ${S_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/shim/atg_telemetry_stub.cpp")
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${outdir}")
  xsl_desktop_settings(${name})
  target_compile_definitions(${name} PRIVATE ${S_DEFINES})
  target_include_directories(${name} PRIVATE ${S_INCLUDES} "${DXTK}/Inc")
  target_link_directories(${name} PRIVATE "${GXDK_ROOT}/windows/lib/x64")
  target_link_libraries(${name} PRIVATE dtk12_desktop d3d12 dxgi dxguid)
  foreach(lib ${S_EXTRA_LIBS})
    string(TOLOWER "${lib}" l)
    if(EXISTS "${GXDK_ROOT}/windows/lib/x64/${lib}" OR l MATCHES "^(advapi32|comdlg32|kernel32|ole32|oleaut32|runtimeobject|shell32|user32|uuid)\\.lib$")
      target_link_libraries(${name} PRIVATE ${lib})
    endif()
  endforeach()
  set(outs)
  foreach(sh ${S_SHADERS})
    string(REPLACE "|" ";" p "${sh}")
    list(GET p 0 file)
    list(GET p 1 profile)
    list(GET p 2 entry)
    list(GET p 3 base)
    list(GET p 4 opts)
    string(REPLACE "@OUTDIR@" "Z:${outdir}/" opts "${opts}")
    separate_arguments(opts WINDOWS_COMMAND "${opts}")
    get_filename_component(dir "${file}" DIRECTORY)
    get_filename_component(fname "${file}" NAME)
    set(out "${outdir}/${base}.cso")
    add_custom_command(OUTPUT "${out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${outdir}"
      COMMAND ${CMAKE_COMMAND} -E env WINEPREFIX=${XSL_TOOL_PREFIX} WINEDEBUG=-all
              ${XSL_WINE} "${XSL_PC_DXC}" -nologo -T ${profile} -E ${entry} ${opts} -I . "${fname}" -Fo "Z:${out}"
      WORKING_DIRECTORY "${dir}" DEPENDS "${file}" COMMENT "dxc_pc ${name}/${base}")
    list(APPEND outs "${out}")
  endforeach()
  foreach(a ${S_ASSETS})
    string(REPLACE "|" ";" p "${a}")
    list(GET p 0 src)
    list(LENGTH p n)
    set(sub "")
    if(n GREATER 1)
      list(GET p 1 sub)
    endif()
    if(EXISTS "${src}")
      get_filename_component(fname "${src}" NAME)
      add_custom_command(OUTPUT "${outdir}/${sub}/${fname}" COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "${outdir}/${sub}/${fname}" DEPENDS "${src}")
      list(APPEND outs "${outdir}/${sub}/${fname}")
    endif()
  endforeach()
  if(outs)
    add_custom_target(${name}_content ALL DEPENDS ${outs})
    add_dependencies(${name} ${name}_content)
  endif()
endfunction()

# Reference pairs: <console sample>=<desktop project>.
set(XSL_REFERENCES "SimpleTriangle=IntroGraphics/SimpleTriangleDesktop" CACHE STRING "Console sample = desktop counterpart")
foreach(pair ${XSL_REFERENCES})
  string(REPLACE "=" ";" p "${pair}")
  list(GET p 1 rel)
  get_filename_component(n "${rel}" NAME)
  if(EXISTS "${XSL_SAMPLES_ROOT}/Samples/${rel}/${n}.vcxproj")
    xsl_add_desktop(${n} "${XSL_SAMPLES_ROOT}/Samples/${rel}/${n}.vcxproj" x64)
  endif()
endforeach()
