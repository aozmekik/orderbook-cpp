include(GoogleTest)
find_package(Threads REQUIRED)

# --- Unit, semantic and differential tests ------------------------------------

add_executable(
  orderbook_tests
  book_test.cpp
  concurrency_test.cpp
  containers_test.cpp
  csv_test.cpp
  differential_test.cpp
  format_and_engine_test.cpp
  histogram_test.cpp
  level_index_test.cpp
  vocabulary_test.cpp
  workload_test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook::orderbook GTest::gtest_main
                                              Threads::Threads)
target_include_directories(orderbook_tests PRIVATE ${PROJECT_SOURCE_DIR}/bench)
ob_enable_warnings(orderbook_tests)
ob_enable_invariant_checks(orderbook_tests)
gtest_discover_tests(orderbook_tests DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 900)

# Replaces the global allocation functions, so it gets its own binary.
add_executable(orderbook_allocation_test allocation_test.cpp)
target_link_libraries(orderbook_allocation_test PRIVATE orderbook::orderbook GTest::gtest_main)
ob_enable_warnings(orderbook_allocation_test)
ob_enable_invariant_checks(orderbook_allocation_test)
gtest_discover_tests(orderbook_allocation_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 900)

# --- Command-line golden tests -----------------------------------------------
# Every engine must print exactly the same bytes for the same input.

if(OB_BUILD_APPS)
  set(golden ${CMAKE_CURRENT_SOURCE_DIR}/golden)
  set(examples ${PROJECT_SOURCE_DIR}/examples)

  function(ob_golden_test name expected)
    foreach(engine IN ITEMS ladder map reference)
      string(JOIN "|" command $<TARGET_FILE:obreplay> --engine=${engine} ${ARGN})
      add_test(NAME cli.${name}.${engine}
               COMMAND ${CMAKE_COMMAND} -DCOMMAND=${command} -DEXPECTED=${golden}/${expected}
                       -P ${golden}/compare.cmake)
    endforeach()
  endfunction()

  ob_golden_test(orders.trades orders.trades.txt ${examples}/orders.csv)
  ob_golden_test(orders.events orders.events.txt --events ${examples}/orders.csv)
  ob_golden_test(stp.cancel_newest self_trade.cancel_newest.txt --events --stp=cancel_newest
                 ${examples}/self_trade.csv)
  ob_golden_test(stp.cancel_oldest self_trade.cancel_oldest.txt --events --stp=cancel_oldest
                 ${examples}/self_trade.csv)

  # Input errors exit with status 1 and name the line and field.
  add_test(NAME cli.parse_error COMMAND obreplay ${golden}/bad.csv)
  set_tests_properties(cli.parse_error PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli.parse_error_message COMMAND obreplay ${golden}/bad.csv)
  set_tests_properties(cli.parse_error_message
                       PROPERTIES PASS_REGULAR_EXPRESSION "bad.csv: line 3, field 6: invalid_number")
  add_test(NAME cli.usage_error COMMAND obreplay --engine=fast ${examples}/orders.csv)
  set_tests_properties(cli.usage_error PROPERTIES WILL_FAIL TRUE)
  add_test(NAME cli.help COMMAND obreplay --help)
  set_tests_properties(cli.help PROPERTIES PASS_REGULAR_EXPRESSION "usage: obreplay")

  # obgen output replays cleanly (and its own summary is printed).
  add_test(NAME cli.obgen_help COMMAND obgen --help)
  set_tests_properties(cli.obgen_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: obgen")
  add_test(NAME cli.obgen_summary COMMAND obgen --commands=2000 --symbols=2)
  set_tests_properties(cli.obgen_summary PROPERTIES PASS_REGULAR_EXPRESSION
                                                    "obgen: 2000 commands over 2 symbol")
endif()
