# Benchmarks

All inputs are synthetic and generated from fixed seeds before timing starts.
The numbers below come from one run on one laptop that other jobs were using
at the same time; treat them as a comparison between the book implementations
on the same machine at the same moment, not as absolute figures.

## What is measured

`bench/bench_orderbook.cpp` (Google Benchmark v1.10.0) runs every benchmark
against up to three books:

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
| `WindowToggle/<d>` | a one-sided book of 3,000 consecutive ask levels; one order `d` ticks better than the best ask is submitted and cancelled alternately (one iteration is one request) |
| `DeepOverflow` | 100,000 ask levels 10,000 ticks and more past the touch, far outside the window; one order on a new far level is added and cancelled alternately |
| `ClockResolution` | the smallest non-zero step of `steady_clock` |

Refilling or rebuilding a book happens with the timer paused, and so does
destroying one. `Add` starts from, and refills with, a book that has already
held the whole batch once, so the first-touch page faults of its pools are not
timed. Event sinks count events, so no work can be optimised away, but they do
not format or store anything. `WindowToggle` and `DeepOverflow` are the known
worst cases of the ladder; they are there to measure its weaknesses, not to
flatter it.

## Results

Command:

```sh
cmake --preset release && cmake --build --preset release
./build/release/bench/orderbook_bench --benchmark_repetitions=10 \
  --benchmark_report_aggregates_only=true --benchmark_out=bench.json --benchmark_out_format=json
```

Machine and toolchain: Apple M1 Pro (8 cores), 16 GB RAM, macOS 26.6.2, Apple
clang 21.0.0 (clang-2100.1.1.101), `-O3 -DNDEBUG` (the `release` preset),
Google Benchmark v1.10.0 built in release mode, library at commit `f8aa1a6`.
Run on 2026-10-18 from 11:46 to 11:52 while other builds were running: the
1-minute load average was 8.9 when the run started and 15.2 when it ended (on
8 cores). 10 repetitions per benchmark.

Because the machine was busy, the throughput tables give the **median CPU
time** per operation (time the process actually ran, so time spent
descheduled is left out) and its **coefficient of variation (CV)** across the
10 repetitions. Wall-clock medians are in the JSON output and were higher and
noisier (wall-clock CV up to 24% on the `Map` rows). A ratio that is not
clearly larger than the noise of its rows is reported as "no measurable
difference".

### Throughput (median CPU time per operation, CV in brackets)

| Benchmark | Ladder | Map | Reference | Map / Ladder |
| --- | ---: | ---: | ---: | --- |
| `Add` | 22.5 ns (5.1%) | 42.0 ns (4.7%) | 72.0 ns (2.6%) | 1.9x |
| `Cancel` | 25.7 ns (2.9%) | 31.4 ns (19.9%) | 3,912 ns (7.7%) | no measurable difference |
| `Match` | 70.6 ns (7.7%) | 93.8 ns (10.8%) | 146.5 ns (7.2%) | no measurable difference |
| `Workload` | 24.9 ns (1.3%) | 33.9 ns (1.0%) | 120.9 ns (0.4%) | 1.4x |
| `WorkloadWide` | 39.6 ns (0.2%) | 67.9 ns (2.4%) | 152.1 ns (2.7%) | 1.7x |

`Cancel` and `Match` are listed as no measurable difference because they do
not reproduce: in a run 11 minutes earlier at a lower load (9.4 falling to
6.5), with the same code except the latency harness, they measured 25.9 ns vs
28.4 ns and 70.9 ns vs 70.9 ns.

### Per-command latency on the `Workload` stream

| Book | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Ladder | 41 ns | 84 ns (CV 15%) | 209 ns (CV 30%) | 141,834 ns |
| Map | 41 ns | 84 ns (CV 21%) | 167 ns (CV 19%) | 135,021 ns |
| Reference | 125 ns | 375 ns (CV 8%) | 799 ns (CV 14%) | 13,361,708 ns |

Percentiles are the medians over the 10 repetitions of each repetition's
percentile (3 million timed commands per repetition). Ladder and Map show no
measurable difference here: they tie at p50 and p99, and their p99.9 values
are within the spread of the repetitions.

### Known worst cases of the ladder (median CPU time per request)

| Benchmark | Ladder | Map |
| --- | ---: | ---: |
| `WindowToggle/600` | 20.3 ns (3.9%) | 48.7 ns (5.5%) |
| `WindowToggle/1600` | 44.4 ns (4.2%) | 43.6 ns (2.4%) |
| `WindowToggle/3000` | 64.0 ns (4.3%) | 42.7 ns (1.4%) |
| `WindowToggle/5000` | 13,806 ns (0.9%) | 42.7 ns (1.4%) |
| `DeepOverflow` | 15,581 ns (0.8%) | 152.2 ns (6.8%) |

With the default 4096-tick window, an order up to 512 ticks (W/8) better than
the best stays inside the window. Further out, the submit moves the window
down and evicts the far end of the book into the overflow; the window moves
back up only after as many requests as levels moved, which is what keeps
`WindowToggle/1600` and `/3000` near the `std::map` cost on average. Before
that rule (commit `007f9cc`), the same benchmarks took 4,464 ns and 10,793 ns
of CPU time per request (one run with `--benchmark_min_time=0.2s`, load
average about 17). Past about W ticks (`/5000`) the cancel empties the ring,
which forces a window move on every request. `DeepOverflow` is the O(n)
vector insert and erase of the overflow: with 100,000 far levels the ladder is
about 100 times slower than the tree there.

## Reading the numbers

- **The clock is coarse.** `ClockResolution` reports 41 ns: on Apple silicon
  `steady_clock` advances in steps of 1/24 MHz, about 41.7 ns. A per-command
  latency is the difference of two clock reads, so it is a whole number of
  ticks (0, 41, 83, 125, 166 ns, ...). A p50 of 41 ns means "at most one
  tick", which is why the books tie there; only the tails carry information.
- **The latency harness costs about as much as the work.** `Latency` runs at
  about 48 ns per command for Ladder against 25 ns in `Workload`: the
  difference is the two clock reads and the histogram update per command.
  The harness is also sensitive to code generation: an earlier version that
  kept the engine in a variable outside the timed loop measured about 15 ns
  more per command for both books, with identical book code.
- **The max is the operating system.** A single command taking over 100 us,
  with the book far below its reserved capacity, is a preemption or an
  interrupt on a loaded machine, not book work.
- **Where the ladder helps.** Creating and removing price levels: `Add`
  (every new price level is a tree node allocation and an O(log n) insert
  for Map) and both workload replays, where cancel-heavy flow near the touch
  creates and empties levels constantly. With most activity outside the
  window (`WorkloadWide`) the ladder still wins, because the touch itself
  stays in the ring and the overflow is a compact sorted vector that this
  stream keeps short; `DeepOverflow` shows what happens when it is not.
- **Where it does not.** `Cancel` and `Match` mostly touch levels that hold
  hundreds of orders, so they rarely create or remove one; the work is the
  id-map lookup, unlinking and event emission, which both books share. And
  the worst cases above.
- **Reference cancel is linear** in the length of the level's queue
  (`std::list` search), which is the price of keeping the oracle obviously
  correct.

## Allocation

`tests/allocation_test.cpp` replaces the global allocation functions and
counts calls while an `OrderBook` processes requests: the default `obgen`
stream and its wide variant (300,000 commands in release builds, 50,000 in
debug builds, with room reserved for 16,384 orders), 200 levels outside the
ladder window, a window move that evicts 500 levels into the overflow, and a
book sized with `reserve()` after construction. All of them perform zero
allocations, while the Map baseline must perform more than 100 on the
default stream (it allocates a tree node per new price level). The test runs
in every CI configuration, sanitizers included.

## Reproducing

Close other heavy programs first, then run the command above. For a single
benchmark, filter it, for example `--benchmark_filter='^Workload/'`. The
JSON written by `--benchmark_out` includes the machine context and the load
average that Google Benchmark records. On Linux, pinning the process to one
core (`taskset -c 2`) and a fixed CPU frequency reduce the noise further.
