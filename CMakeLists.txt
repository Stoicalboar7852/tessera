# Tessera's own native Windows components (helpers, tools, tests).
#
# The upstream-derived host in host/ has its own CMake project and is built with MSYS2 UCRT64 (MinGW-GCC),
# the VDD in drivers/vdd is built with the WDK. This project builds with MSVC (the normal case) and with
# MinGW-GCC, so the code shared with the host (host-helpers/common) is checked against both toolchains.
cmake_minimum_required(VERSION 3.25)
project(tessera_native VERSION 0.0.1 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Tessera native components are Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TESSERA_BUILD_TESTS "Build unit tests" ON)
option(TESSERA_WERROR "Treat warnings as errors" OFF)

include(cmake/tessera_dependencies.cmake)
include(cmake/tessera_compile_options.cmake)

add_subdirectory(host-helpers)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/recover/CMakeLists.txt")
  add_subdirectory(tools/recover)
endif()

if(TESSERA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(host-helpers/tests)
endif()
