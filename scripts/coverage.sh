#!/usr/bin/env bash
# Line and branch coverage of the library (include/ and src/) under the test
# suite, using Clang source-based coverage.
#
#   scripts/coverage.sh [LLVM_BIN_DIR]
#
# LLVM_BIN_DIR must contain clang++, llvm-profdata and llvm-cov of the same
# release (default: $(brew --prefix llvm)/bin on macOS, /usr/bin on Linux).
# Writes a summary to standard output and an HTML report to
# build/coverage/html/index.html.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -ge 1 ]]; then
  llvm="$1"
elif command -v brew >/dev/null 2>&1; then
  llvm="$(brew --prefix llvm)/bin"
else
  llvm=/usr/bin
fi
build="$root/build/coverage"

cd "$root"
rm -rf "$build/profiles"
CXX="$llvm/clang++" cmake --preset coverage >/dev/null
cmake --build --preset coverage >&2
ctest --preset coverage --parallel 4 >/dev/null

"$llvm/llvm-profdata" merge -sparse "$build"/profiles/*.profraw -o "$build/merged.profdata"
objects=(
  "$build/tests/orderbook_tests"
  -object "$build/tests/orderbook_allocation_test"
  -object "$build/apps/obreplay"
  -object "$build/apps/obgen"
  -object "$build/examples/example_basic"
  -object "$build/examples/example_market_data"
)
"$llvm/llvm-cov" report "${objects[@]}" -instr-profile="$build/merged.profdata" \
  --show-branch-summary "$root/include" "$root/src"
"$llvm/llvm-cov" show "${objects[@]}" -instr-profile="$build/merged.profdata" \
  -format=html -output-dir="$build/html" "$root/include" "$root/src"
