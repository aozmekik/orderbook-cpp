# libFuzzer targets. Configure with a Clang that ships libFuzzer, e.g.
#   CXX=$(brew --prefix llvm)/bin/clang++ cmake --preset fuzz     (macOS)
#   CXX=clang++ cmake --preset fuzz                                (Linux)
# The preset compiles everything with -fsanitize=fuzzer-no-link,address,undefined
# and keeps assertions on. Seed corpora: corpus/csv for fuzz_csv, corpus/engine
# for fuzz_engine (whose inputs start with a policy byte).

foreach(target IN ITEMS fuzz_csv fuzz_engine)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE orderbook::orderbook)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  # Every book re-checks its invariants after every request, whatever the build type.
  target_compile_definitions(${target} PRIVATE OB_ENABLE_INVARIANT_CHECKS=1)
  ob_enable_warnings(${target})
endforeach()
