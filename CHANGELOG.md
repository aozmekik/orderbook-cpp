# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Until 1.0.0, minor versions may change the API.

## [Unreleased]

## [0.1.0] - 2026-10-18

### Added

- `OrderBook`: single-symbol limit order book with price-time priority over
  integer prices (ticks) and quantities (lots); limit, market, IOC, FOK and
  post-only (reject) orders; cancel; modify with exchange priority rules; and
  self-trade prevention (`cancel_newest`, `cancel_oldest`).
- Events: `Accepted`, `Rejected` (with reason), `Fill` (taker and maker ids,
  side, price, quantity and both remainders), `Cancelled` (with reason),
  `Modified` and level-2 `BookDelta`; top-of-book and depth queries.
- `LadderLevels`: tick-indexed ring of price levels around the touch with an
  occupancy bitmap and a sorted overflow, whose voluntary window moves are
  delayed so that they cost amortised O(1); `MapLevels`, the `std::map`
  baseline, behind the same `LevelIndex` concept (including `reserve`).
- Index-addressed object pools, intrusive FIFO queues per price level and an
  open-addressing order-id map, so that an `OrderBook` within its reserved
  capacity does not allocate, wherever its price levels are.
- A per-book seed for the order-id hash (`BookConfig::hash_seed`, random by
  default), so that clients cannot choose colliding ids.
- `ReferenceBook`, a deliberately simple implementation used as the oracle in
  differential tests, and `BasicEngine` for many symbols.
- Invariant checks after every operation (`OB_ENABLE_INVARIANT_CHECKS`), off
  by default and on in this project's unoptimised builds.
- `ob::expected`, a fallback for `std::expected`; the CMake option
  `OB_USE_STD_EXPECTED` makes it `std::expected` (C++23), recorded at build
  time in the generated `<orderbook/config.hpp>`.
- CSV order-stream reader and writer; deterministic text rendering of events
  and snapshots.
- `obreplay` (replay a CSV order stream, print trades and the final book) and
  `obgen` (synthetic, cancel-heavy, near-touch order flow, with
  `workload::config_error` validating its settings).
- Tests: typed semantic tests for all three books, randomized differential
  tests, allocation, concurrency, golden-file and exit-status CLI tests;
  libFuzzer targets for the CSV parser and the engines; Google Benchmark
  suite with latency percentiles and the ladder's known worst cases.
- CMake presets (`dev`, `release`, `cxx23`, `asan-ubsan`, `tsan`, `fuzz`,
  `coverage`), install rules with a CMake package config, and a CI workflow
  for Ubuntu (GCC 14, Clang 18) and macOS.

[Unreleased]: https://github.com/aozmekik/orderbook-cpp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/aozmekik/orderbook-cpp/releases/tag/v0.1.0
