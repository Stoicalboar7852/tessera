include(GoogleTest)

add_executable(tessera_common_tests
  display_modes_test.cpp
  display_topology_test.cpp
  multistream_test.cpp
  state_journal_test.cpp)
target_link_libraries(tessera_common_tests PRIVATE tessera_common GTest::gtest_main)
# Test vectors shared with common-c's tests.
target_include_directories(tessera_common_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../protocol/tests")
tessera_apply_options(tessera_common_tests)
gtest_discover_tests(tessera_common_tests DISCOVERY_MODE PRE_TEST)
