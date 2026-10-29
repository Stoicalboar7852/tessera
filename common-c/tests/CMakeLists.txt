# Tests for Tessera's additions to moonlight-common-c. A separate project, so the upstream CMakeLists.txt stays as is:
#   cmake -S common-c/tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.16)
project(tessera_common_c_tests LANGUAGES C)

# Static, so the tests can reach the library's internal functions on every platform.
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(.. common-c)

add_executable(tessera_common_c_tests tessera_test.c)
target_link_libraries(tessera_common_c_tests PRIVATE moonlight-common-c enet)
# The tests reach internal state (Limelight-internal.h), which needs the library's own definitions.
target_compile_definitions(tessera_common_c_tests PRIVATE HAS_SOCKLEN_T)
# Test vectors shared with the host's tests.
target_include_directories(tessera_common_c_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/tests")
if(MSVC)
  target_compile_options(tessera_common_c_tests PRIVATE /W3 /WX)
else()
  target_compile_options(tessera_common_c_tests PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME tessera_common_c_tests COMMAND tessera_common_c_tests)

# The copies of the video path must not quietly share stream 0's state (src/TesseraVideoInstance.h).
if(CMAKE_NM)
  add_test(NAME tessera_instance_symbols
    COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
      -DALLOWLIST=${CMAKE_CURRENT_SOURCE_DIR}/instance_symbols.txt
      "-DOBJECTS=$<TARGET_OBJECTS:moonlight-common-c>"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_instance_symbols.cmake)
endif()
