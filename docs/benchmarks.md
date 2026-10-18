# Benchmarks

All inputs are synthetic and generated from fixed seeds before timing starts.
The numbers below are from one run on one laptop under heavy background load;
treat them as a comparison between the three book implementations on the same
machine at the same time, not as absolute figures.

## What is measured

`bench/bench_orderbook.cpp` (Google Benchmark v1.10.0) runs every benchmark
against three books:

- **Ladder**: `ob::OrderBook`, price levels in `LadderLevels` (tick-indexed
  ring around the touch, see [ADR 0001](adr/0001-price-level-ladder.md)).
- **Map**: `ob::MapOrderBook`, the same book with `std::map` price levels.
  Pools, queues, the id map and all event code are shared with Ladder, so the
  difference between the two is the level structure alone.
- **Reference**: `ob::ReferenceBook`, the `std::map` + `std::list` +
  `std::unordered_map` oracle used by the differential tests. It is slow on
  purpose and is shown only for scale.

| Benchmark | One iteration is |
| --- | --- |
| `Add` | one passive limit order into a book that grows from empty to 65,536 orders; prices 0 to 127 ticks behind the touch with probability proportional to 1/(d+1) |
| `Cancel` | one cancel, in random order, of the 65,536 orders above |
| `Match` | one market buy of 100 to 500 lots against an ask side of 100-lot orders (each fills one to five makers) |
| `Workload` | one command of the default `obgen` stream (seed 1, 1,000,000 commands, 55% new, 42% cancel, 3% modify; 90.2% of the orders that leave the book are cancelled) |
| `WorkloadWide` | the same with `--max-distance=20000`: most passive orders land outside the 4096-tick ladder window, in its sorted overflow |
| `Latency` | three passes over the `Workload` stream, timing every command with two `std::chrono::steady_clock` reads and recording it in an HDR-style log-linear histogram (`bench/latency_histogram.hpp`, at most 1/64 relative error) |
| `ClockResolution` | the smallest non-zero step of `steady_clock` |

Refilling a book between batches happens with the timer paused. Event sinks
count events, so no work can be optimised away, but they do not format or
store anything.

## Results

Command:

```sh
cmake --preset release && cmake --build --preset release
./build/release/bench/orderbook_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Machine and toolchain: Apple M1 Pro (8 cores), 16 GB RAM, macOS 26.6.2, Apple
clang 21.0.0 (clang-2100.1.1.101), `-O3 -DNDEBUG` (the `release` preset),
Google Benchmark v1.10.0 built in release mode. Run on 2026-10-18 while other
builds were running on the same machine: the load average was 33.1 (1 min),
37.8 (5 min) and 38.4 (15 min) when the run started, so every number carries
contention noise. Medians of 5 repetitions; the coefficient of variation
across repetitions was at most 4.3%.

### Throughput (median time per operation)

| Benchmark | Ladder | Map | Reference | Map / Ladder |
| --- | ---: | ---: | ---: | ---: |
| `Add` | 22.3 ns | 39.9 ns | 73.9 ns | 1.79x |
| `Cancel` | 24.6 ns | 26.5 ns | 3,787 ns | 1.08x |
| `Match` | 75.1 ns | 76.8 ns | 137.6 ns | 1.02x |
| `Workload` | 24.3 ns | 34.1 ns | 123.4 ns | 1.40x |
| `WorkloadWide` | 39.5 ns | 69.4 ns | 151.3 ns | 1.76x |

### Per-command latency on the `Workload` stream

| Book | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Ladder | 41 ns | 83 ns | 125 ns | 40,791 ns |
| Map | 41 ns | 84 ns | 167 ns | 41,125 ns |
| Reference | 125 ns | 335 ns | 751 ns | 437,667 ns |

Percentiles are the medians over the 5 repetitions of each repetition's
percentile (3 million timed commands per repetition).

## Reading the numbers

- **The clock is coarse.** `ClockResolution` reports 41 ns: on Apple silicon
  `steady_clock` advances in steps of 1/24 MHz, about 41.7 ns. A per-command
  latency is the difference of two clock reads, so it is a whole number of
  ticks (0, 41, 83, 125, 166 ns, ...). A p50 of 41 ns means "at most one
  tick", which is why Ladder and Map tie there; the tail percentiles are the
  informative part. Two clock reads also add about 22 ns per command
  (`Latency` runs at 47 ns per command, `Workload` at 24 ns).
- **The max is the operating system.** A single command taking 40 us, with
  the book far below its reserved capacity, is a preemption or an interrupt
  on a loaded machine, not book work.
- **Where the ladder helps.** Creating and removing price levels: `Add`
  (every new price level is a tree node allocation and an O(log n) insert
  for Map) and both workload replays, where cancel-heavy flow near the touch
  creates and empties levels constantly. With most activity outside the
  window (`WorkloadWide`) the ladder still wins, because the touch itself
  stays in the ring and the overflow is a compact sorted vector.
- **Where it does not.** `Cancel` and `Match` mostly touch levels that hold
  hundreds of orders, so they rarely create or remove one; the work is the
  id-map lookup, unlinking and event emission, which both books share.
- **Reference cancel is linear** in the length of the level's queue
  (`std::list` search), which is the price of keeping the oracle obviously
  correct.

## Allocation

`tests/allocation_test.cpp` replaces the global allocation functions and
counts calls while an engine replays a synthetic `obgen` stream (300,000
commands in release builds, 50,000 in debug builds): the Ladder book with
room reserved for 16,384 orders performs zero allocations, while the Map
baseline must perform more than 100 (it allocates a tree node per new price
level). The test runs in every CI configuration, sanitizers included.

## Reproducing

Close other heavy programs first, then run the command above. For a single
benchmark, filter it, for example
`--benchmark_filter='^Workload/'`. `--benchmark_out=results.json
--benchmark_out_format=json` writes the raw data, including the machine
context Google Benchmark records.
