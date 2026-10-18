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

# Replaces the global allocation functions, so it gets its own binary. The
# ThreadSanitizer runtime defines the same functions as strong symbols, so the
# test cannot link there; every other build, the ASan/UBSan one included, runs it.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=([^ ]*,)?thread")
  message(STATUS "ThreadSanitizer build: skipping orderbook_allocation_test")
else()
  add_executable(orderbook_allocation_test allocation_test.cpp)
  target_link_libraries(orderbook_allocation_test PRIVATE orderbook::orderbook GTest::gtest_main)
  ob_enable_warnings(orderbook_allocation_test)
  ob_enable_invariant_checks(orderbook_allocation_test)
  gtest_discover_tests(orderbook_allocation_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 900)
endif()

# --- Command-line tests ------------------------------------------------------
# Golden files: every engine must print exactly the same bytes for the same input.

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

  # Exact exit statuses: 0 on success, 1 for input that cannot be read or
  # parsed, 2 for usage errors. A crash fails these tests.
  set(expect ${CMAKE_CURRENT_SOURCE_DIR}/cli/expect_status.cmake)
  function(ob_status_test name status match)
    string(JOIN "|" command ${ARGN})
    add_test(NAME cli.${name} COMMAND ${CMAKE_COMMAND} -DCOMMAND=${command} -DSTATUS=${status}
                                      -DMATCH=${match} -P ${expect})
  endfunction()
  set(obreplay $<TARGET_FILE:obreplay>)
  set(obgen $<TARGET_FILE:obgen>)

  ob_status_test(parse_error 1 "bad.csv: line 3, field 6: invalid_number" ${obreplay}
                 ${golden}/bad.csv)
  ob_status_test(missing_input 1 "cannot read .*no-such-file.csv" ${obreplay}
                 ${golden}/no-such-file.csv)
  ob_status_test(directory_input 1 "is a directory" ${obreplay} ${golden})
  ob_status_test(usage_error 2 "--engine must be" ${obreplay} --engine=fast ${examples}/orders.csv)
  ob_status_test(unknown_option 2 "unknown option --fast" ${obreplay} --fast
                 ${examples}/orders.csv)
  ob_status_test(flag_with_value 2 "--events takes no value" ${obreplay} --events=yes
                 ${examples}/orders.csv)
  ob_status_test(option_without_value 2 "--depth needs a value" ${obreplay} --depth
                 ${examples}/orders.csv)
  ob_status_test(zero_ladder_ticks 2 "--ladder-ticks needs a positive integer" ${obreplay}
                 --ladder-ticks=0 ${examples}/orders.csv)
  ob_status_test(missing_input_argument 2 "missing input file" ${obreplay})
  # A huge level limit costs memory for the levels shown only.
  ob_status_test(huge_depth 0 "snapshot MSFT bids=1 asks=1" ${obreplay} --depth=99999999999
                 ${examples}/orders.csv)
  ob_status_test(help 0 "usage: obreplay" ${obreplay} --help)

  ob_status_test(obgen_help 0 "usage: obgen" ${obgen} --help)
  ob_status_test(obgen_summary 0 "obgen: 2000 commands over 2 symbol" ${obgen} --commands=2000
                 --symbols=2)
  ob_status_test(obgen_start_price_out_of_range 2 "start price must be between" ${obgen}
                 --start-price=9223372036854775807)
  ob_status_test(obgen_unknown_option 2 "unknown option --fast" ${obgen} --fast)

  # obgen output replays cleanly, and identically through every engine.
  add_test(NAME cli.obgen_replay
           COMMAND ${CMAKE_COMMAND} -DOBGEN=${obgen} -DOBREPLAY=${obreplay}
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/obgen_replay
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/obgen_replay.cmake)
endif()
