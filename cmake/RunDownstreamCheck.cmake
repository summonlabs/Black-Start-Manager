# Configures, builds, and runs the out-of-tree downstream consumer against an installed
# Black Start Manager package. Invoked by CTest when
# BLACK_START_MANAGER_DOWNSTREAM_PREFIX is set, and by the release verification steps.
#
# A nested CMake project needs a usable C++ toolchain in this process environment. On
# Windows with MSVC that environment is produced by vcvars64.bat, so this script locates
# it through vswhere (never through a hard-coded path) and runs the nested commands
# through it. When no toolchain can be reached the check fails with the exact reason
# rather than reporting a pass it did not earn.

foreach(required BSM_SOURCE_DIR BSM_BINARY_DIR BSM_PREFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "RunDownstreamCheck.cmake requires -D${required}=...")
  endif()
endforeach()

set(bsm_vcvars "")
if(WIN32)
  set(bsm_program_files_x86 "$ENV{ProgramFiles\(x86\)}")
  set(bsm_vswhere "${bsm_program_files_x86}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(EXISTS "${bsm_vswhere}")
    execute_process(COMMAND "${bsm_vswhere}" -latest -products * -requires
                            Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                            -property installationPath
                    OUTPUT_VARIABLE bsm_vs_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT bsm_vs_path STREQUAL "")
      set(bsm_vcvars "${bsm_vs_path}/VC/Auxiliary/Build/vcvars64.bat")
    endif()
  endif()
endif()

# Runs one nested command, entering the MSVC developer environment first when one was
# found. The environment is entered through a generated batch file rather than through
# cmd /c "call ... && ...", because nesting quotes inside an outer quoted command line is
# not reliable.
set(bsm_run_serial 0)
function(bsm_run description)
  math(EXPR bsm_run_serial "${bsm_run_serial} + 1")
  if(WIN32 AND NOT bsm_vcvars STREQUAL "" AND EXISTS "${bsm_vcvars}")
    set(bsm_batch "${BSM_BINARY_DIR}/bsm-run-${bsm_run_serial}.bat")
    set(bsm_script "@echo off\r\ncall \"${bsm_vcvars}\" >nul 2>&1\r\n")
    foreach(argument IN LISTS ARGN)
      set(bsm_script "${bsm_script}\"${argument}\" ")
    endforeach()
    set(bsm_script "${bsm_script}\r\nexit /b %ERRORLEVEL%\r\n")
    file(WRITE "${bsm_batch}" "${bsm_script}")
    execute_process(COMMAND cmd /c "${bsm_batch}"
      RESULT_VARIABLE bsm_result
      OUTPUT_VARIABLE bsm_output
      ERROR_VARIABLE bsm_output)
  else()
    execute_process(COMMAND ${ARGN}
      RESULT_VARIABLE bsm_result
      OUTPUT_VARIABLE bsm_output
      ERROR_VARIABLE bsm_output)
  endif()

  if(NOT bsm_result EQUAL 0)
    if(bsm_output MATCHES "No CMAKE_CXX_COMPILER could be found" OR
       bsm_output MATCHES "CMAKE_CXX_COMPILER not set")
      message(FATAL_ERROR
              "${description} could not run because no C++ compiler is reachable from this "
              "environment. Run the suite from a developer environment (for MSVC, after "
              "vcvars64.bat) to execute the installed-package check for real.\n${bsm_output}")
    endif()
    if(bsm_output MATCHES "Could not find a package configuration file provided by \"BlackStartManager\"")
      message(FATAL_ERROR
              "${description} failed because no Black Start Manager package was found under "
              "'${BSM_PREFIX}'. Install the project into that prefix first, for example "
              "'cmake --install <build-dir> --prefix ${BSM_PREFIX}'.\n${bsm_output}")
    endif()
    message(FATAL_ERROR "${description} failed with exit ${bsm_result}:\n${bsm_output}")
  endif()
  set(bsm_last_output "${bsm_output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${BSM_BINARY_DIR}")
file(MAKE_DIRECTORY "${BSM_BINARY_DIR}")

set(configure_command "${CMAKE_COMMAND}"
  -S "${BSM_SOURCE_DIR}/downstream/consumer"
  -B "${BSM_BINARY_DIR}"
  "-DCMAKE_PREFIX_PATH=${BSM_PREFIX}")
if(DEFINED BSM_GENERATOR AND NOT BSM_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${BSM_GENERATOR}")
endif()
if(DEFINED BSM_CONFIG AND NOT BSM_CONFIG STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${BSM_CONFIG}")
endif()

bsm_run("downstream configure" ${configure_command})
bsm_run("downstream build" "${CMAKE_COMMAND}" --build "${BSM_BINARY_DIR}")
bsm_run("downstream run" "${CMAKE_COMMAND}" --build "${BSM_BINARY_DIR}" --target run_consumer)

message(STATUS "downstream consumer output:\n${bsm_last_output}")
