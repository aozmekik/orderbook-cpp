# libFuzzer targets. Configure with a Clang that ships libFuzzer, e.g.
#   CXX=$(brew --prefix llvm)/bin/clang++ cmake --preset fuzz     (macOS)
#   CXX=clang++ cmake --preset fuzz                                (Linux)
# The preset compiles everything with -fsanitize=fuzzer-no-link,address,undefined.

foreach(target IN ITEMS fuzz_csv fuzz_engine)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE orderbook::orderbook)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  ob_enable_warnings(${target})
endforeach()
