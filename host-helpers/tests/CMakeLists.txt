include(GoogleTest)

add_executable(tessera_common_tests
  display_topology_test.cpp
  state_journal_test.cpp)
target_link_libraries(tessera_common_tests PRIVATE tessera_common GTest::gtest_main)
tessera_apply_options(tessera_common_tests)
gtest_discover_tests(tessera_common_tests DISCOVERY_MODE PRE_TEST)
