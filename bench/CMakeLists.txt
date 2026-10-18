add_executable(orderbook_bench bench_orderbook.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook::orderbook benchmark::benchmark)
target_include_directories(orderbook_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ob_enable_warnings(orderbook_bench)

# Smoke test: the micro-benchmarks run (briefly) without crashing. Not a measurement,
# and only in optimised builds: debug builds check every invariant after every
# operation, which makes refilling the benchmark books quadratic.
if(OB_BUILD_TESTS AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  add_test(NAME bench.smoke COMMAND orderbook_bench --benchmark_min_time=0.01s
                                    "--benchmark_filter=^(Add|Cancel|Match)/")
endif()
