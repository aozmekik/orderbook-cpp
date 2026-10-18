# Contributing

Thanks for taking a look. Issues and pull requests are welcome; for anything
larger than a small fix, please open an issue first so we can agree on the
semantics (they are part of the contract, see
[docs/semantics.md](docs/semantics.md)).

## Requirements

- CMake 3.25 or newer and Ninja
- A C++20 compiler. CI builds with GCC 14 and Clang 18 on Ubuntu 24.04 and
  with the default Apple Clang of the `macos-latest` runner; development
  happened on Apple clang 21 and Homebrew LLVM 23.
- Python 3.11+ for the pinned formatting and lint tools
- For fuzzing and coverage: a Clang that ships libFuzzer and `llvm-cov`
  (Homebrew `llvm` on macOS, `clang-18` plus `libclang-rt-18-dev` on Ubuntu)

GoogleTest and Google Benchmark are downloaded by CMake (`FetchContent`) at
pinned versions and SHA-256 hashes; nothing is installed globally.

## Build and test

```sh
cmake --preset dev                 # Debug, invariant checks after every operation
cmake --build --preset dev
ctest --preset dev --parallel 4

cmake --workflow --preset release  # configure, build and test the optimised build
```

| Preset | Purpose |
| --- | --- |
| `dev` | Debug build; every book re-checks all of its invariants after every request |
| `release` | `-O3 -DNDEBUG`; used for benchmarks; also runs a benchmark smoke test |
| `cxx23` | Debug build in C++23 mode, which switches `ob::expected` to `std::expected` |
| `asan-ubsan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (the concurrency tests run engines on several threads) |
| `fuzz` | libFuzzer targets; needs `CXX` set to a Clang with libFuzzer |
| `coverage` | Clang source-based coverage; use `scripts/coverage.sh` |

The randomized differential test runs 100,000 requests per configuration in
debug builds and 250,000 in release builds; set `OB_DIFF_OPS` to change that:

```sh
OB_DIFF_OPS=2000000 ./build/release/tests/orderbook_tests --gtest_filter='RandomStreams/*'
```

## Format and lint

The exact tool versions CI uses are pinned in `requirements-dev.txt`:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt

# Format check (or drop --dry-run --Werror and add -i to fix).
git ls-files -- '*.cpp' '*.hpp' | xargs .venv/bin/clang-format --dry-run --Werror

# clang-tidy needs a configured build for compile_commands.json.
cmake --preset dev
.venv/bin/run-clang-tidy.py -p build/dev -j 4 -quiet \
  -clang-tidy-binary "$PWD/.venv/bin/clang-tidy" \
  -extra-arg="-isysroot$(xcrun --show-sdk-path)" \
  "$PWD/(src|apps|bench|tests|examples)/"
```

On Linux, drop the `-extra-arg` line. The configuration (`.clang-tidy`) enables
the `bugprone`, `cert`, `cppcoreguidelines`, `misc`, `modernize`,
`performance`, `portability` and `readability` groups as errors; the few
disabled checks are listed with the reason in the file itself. New code should
not need `NOLINT`; if it does, say why on the same line.

Workflow files are checked with [actionlint](https://github.com/rhysd/actionlint):

```sh
actionlint
```

## Fuzzing

```sh
CXX=$(brew --prefix llvm)/bin/clang++ cmake --preset fuzz   # Linux: CXX=clang++-18
cmake --build --preset fuzz
mkdir -p /tmp/corpus-csv /tmp/corpus-engine
./build/fuzz/fuzz/fuzz_csv    -max_total_time=60 /tmp/corpus-csv    fuzz/corpus
./build/fuzz/fuzz/fuzz_engine -max_total_time=60 /tmp/corpus-engine fuzz/corpus
```

The first directory collects new inputs; `fuzz/corpus` is the checked-in seed
corpus. A crash writes `crash-<hash>` into the current directory; reproduce it
with `./build/fuzz/fuzz/fuzz_engine crash-<hash>`, then add a regression test.

## Coverage

```sh
scripts/coverage.sh "$(brew --prefix llvm)/bin"   # Linux: scripts/coverage.sh /usr/lib/llvm-18/bin
```

It prints line, function and branch coverage for `include/` and `src/` and
writes an HTML report to `build/coverage/html/index.html`.

## Benchmarks

```sh
cmake --preset release && cmake --build --preset release
./build/release/bench/orderbook_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

See [docs/benchmarks.md](docs/benchmarks.md) for what each benchmark measures
and how to read the numbers. Close other heavy programs first and report the
machine, compiler and load average with any number you quote.

## Changing the semantics

A behaviour change has to be made in both `BasicOrderBook` and `ReferenceBook`
(the differential tests compare them event by event), described in
`docs/semantics.md`, pinned by a typed test in `tests/book_test.cpp`, and, if
it changes `obreplay` output, reflected in `tests/golden/`. Regenerate a golden
file with, for example:

```sh
./build/dev/apps/obreplay --events examples/orders.csv > tests/golden/orders.events.txt
```

and review the diff line by line.

## Updating dependencies

Dependabot updates the GitHub Actions and the pinned Python tools. It cannot
update the `FetchContent` pins in `cmake/Dependencies.cmake`; to bump
GoogleTest or Google Benchmark, change the URL, then the `URL_HASH` to the
SHA-256 of the new archive (`curl -sL <url> | shasum -a 256`).

## Commits

[Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`,
`test:`, `docs:`, `build:`, `ci:` ...), one logical change per commit, and
`ctest` green on the `dev` and `release` presets before you push.
