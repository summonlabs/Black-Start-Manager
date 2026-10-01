cmake_minimum_required(VERSION 3.21)

project(BlackStartManager
  VERSION 1.0.0
  DESCRIPTION "Dependency-gated black-start authority protocol for facility restoration"
  HOMEPAGE_URL "https://github.com/summonlabs/Black-Start-Manager"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(BLACK_START_MANAGER_IS_TOP_LEVEL ON)
else()
  set(BLACK_START_MANAGER_IS_TOP_LEVEL OFF)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(BLACK_START_MANAGER_IS_TOP_LEVEL)
  option(BLACK_START_MANAGER_BUILD_TESTS "Build the Black Start Manager test suite" ON)
  option(BLACK_START_MANAGER_BUILD_EXAMPLES "Build the Black Start Manager examples" ON)
  option(BLACK_START_MANAGER_BUILD_BENCHMARKS "Build the Black Start Manager benchmarks" ON)
  option(BLACK_START_MANAGER_BUILD_TOOLS "Build the Black Start Manager command line tools" ON)
else()
  option(BLACK_START_MANAGER_BUILD_TESTS "Build the Black Start Manager test suite" OFF)
  option(BLACK_START_MANAGER_BUILD_EXAMPLES "Build the Black Start Manager examples" OFF)
  option(BLACK_START_MANAGER_BUILD_BENCHMARKS "Build the Black Start Manager benchmarks" OFF)
  option(BLACK_START_MANAGER_BUILD_TOOLS "Build the Black Start Manager command line tools" OFF)
endif()

option(BLACK_START_MANAGER_WARNINGS_AS_ERRORS
  "Treat first-party compiler warnings as errors" ON)
option(BLACK_START_MANAGER_ENABLE_ASAN
  "Build with AddressSanitizer where the toolchain supports it" OFF)

set(BLACK_START_MANAGER_DOWNSTREAM_PREFIX "" CACHE PATH
  "Install prefix of a previous Black Start Manager installation; enables the downstream consumer test")

# Durable store format version. Independent of the package version: it changes only
# when the persisted byte layout or the canonical state encoding changes.
set(BLACK_START_MANAGER_STATE_FORMAT_VERSION 1)

# ---------------------------------------------------------------------------
# First-party compile options
# ---------------------------------------------------------------------------
function(black_start_manager_apply_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(BLACK_START_MANAGER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(BLACK_START_MANAGER_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /fsanitize=address /INCREMENTAL:NO)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wnon-virtual-dtor -Wcast-qual -Wdouble-promotion)
    if(BLACK_START_MANAGER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(BLACK_START_MANAGER_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/black_start_manager/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/black_start_manager/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(black_start_manager
  src/error.cpp
  src/digest.cpp
  src/canonical.cpp
  src/clock.cpp
  src/ids.cpp
  src/enums.cpp
  src/authority.cpp
  src/evidence.cpp
  src/plan.cpp
  src/attempt.cpp
  src/session.cpp
  src/codec_model.cpp
  src/report.cpp
  src/manager.cpp
  src/controller_synthetic.cpp
  src/controller_remote.cpp
  src/store.cpp
  src/detail/paths.cpp
  src/detail/file_io.cpp
  src/detail/process_lock.cpp
  src/detail/socket.cpp
  src/detail/crash_point.cpp)
add_library(BlackStartManager::black_start_manager ALIAS black_start_manager)

set(BLACK_START_MANAGER_PUBLIC_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(BLACK_START_MANAGER_BUILD_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(BLACK_START_MANAGER_PRIVATE_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_include_directories(black_start_manager
  PUBLIC
    "$<BUILD_INTERFACE:${BLACK_START_MANAGER_PUBLIC_INCLUDE}>"
    "$<BUILD_INTERFACE:${BLACK_START_MANAGER_BUILD_INCLUDE}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${BLACK_START_MANAGER_PRIVATE_INCLUDE}")

target_compile_features(black_start_manager PUBLIC cxx_std_20)
target_compile_definitions(black_start_manager
  PRIVATE "BLACK_START_MANAGER_STATE_FORMAT_VERSION=${BLACK_START_MANAGER_STATE_FORMAT_VERSION}")

if(WIN32)
  target_link_libraries(black_start_manager PRIVATE ws2_32)
endif()

black_start_manager_apply_strict_options(black_start_manager)

set_target_properties(black_start_manager PROPERTIES
  OUTPUT_NAME "black_start_manager"
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

if(BUILD_SHARED_LIBS AND WIN32)
  set_target_properties(black_start_manager PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
# Command line tools
# ---------------------------------------------------------------------------
if(BLACK_START_MANAGER_BUILD_TOOLS)
  add_executable(bsm_cli tools/bsm_cli.cpp)
  set_target_properties(bsm_cli PROPERTIES OUTPUT_NAME "bsm")
  target_link_libraries(bsm_cli PRIVATE black_start_manager)
  black_start_manager_apply_strict_options(bsm_cli)

  add_executable(bsm_plant tools/bsm_plant.cpp)
  set_target_properties(bsm_plant PROPERTIES OUTPUT_NAME "bsm_plant")
  target_link_libraries(bsm_plant PRIVATE black_start_manager)
  black_start_manager_apply_strict_options(bsm_plant)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS black_start_manager
  EXPORT BlackStartManagerTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(BLACK_START_MANAGER_BUILD_TOOLS)
  install(TARGETS bsm_cli bsm_plant RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/black_start_manager"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/black_start_manager/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/black_start_manager")

set(BLACK_START_MANAGER_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/BlackStartManager")

install(EXPORT BlackStartManagerTargets
  NAMESPACE BlackStartManager::
  FILE BlackStartManagerTargets.cmake
  DESTINATION "${BLACK_START_MANAGER_INSTALL_CMAKEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BlackStartManagerConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/BlackStartManagerConfig.cmake"
  INSTALL_DESTINATION "${BLACK_START_MANAGER_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/BlackStartManagerConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/BlackStartManagerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/BlackStartManagerConfigVersion.cmake"
  DESTINATION "${BLACK_START_MANAGER_INSTALL_CMAKEDIR}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/BlackStartManager")

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
if(BLACK_START_MANAGER_IS_TOP_LEVEL AND (BLACK_START_MANAGER_BUILD_TESTS OR BLACK_START_MANAGER_BUILD_EXAMPLES))
  enable_testing()
endif()

if(BLACK_START_MANAGER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BLACK_START_MANAGER_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(BLACK_START_MANAGER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
