# Tests for Tessera's additions to moonlight-common-c. A separate project, so the upstream CMakeLists.txt stays as is:
#   cmake -S common-c/tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.16)
project(tessera_common_c_tests LANGUAGES C)

# Static, so the tests can reach the library's internal functions on every platform.
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(.. common-c)

add_executable(tessera_common_c_tests tessera_test.c)
target_link_libraries(tessera_common_c_tests PRIVATE moonlight-common-c)
# Test vectors shared with the host's tests.
target_include_directories(tessera_common_c_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/tests")
if(MSVC)
  target_compile_options(tessera_common_c_tests PRIVATE /W3 /WX)
else()
  target_compile_options(tessera_common_c_tests PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME tessera_common_c_tests COMMAND tessera_common_c_tests)
