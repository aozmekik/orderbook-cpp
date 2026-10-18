// SPDX-License-Identifier: MIT
// Throughput and latency of the three book implementations:
//   Ladder    - OrderBook (tick-indexed ring around the touch)
//   Map       - MapOrderBook (the same book with std::map price levels)
//   Reference - ReferenceBook (std::map + std::list + std::unordered_map)
//
// All inputs are synthetic and generated from fixed seeds before timing.
// Run the release build: ./build/release/bench/orderbook_bench

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <benchmark/benchmark.h>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/random.hpp>
#include <orderbook/reference_book.hpp>
#include <orderbook/types.hpp>
#include <orderbook/workload.hpp>

#include "latency_histogram.hpp"

namespace {

using ob::NewOrder;
using ob::OrderId;
using ob::OrderType;
using ob::OwnerId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::SymbolId;

/// Counts events so that the work cannot be optimised away.
struct CountingSink {
  std::uint64_t events = 0;

  template <class E>
  void operator()(const E& /*event*/) noexcept {
    ++events;
  }
};

constexpr std::size_t kBatch = 1U << 16U;  // Orders per refill in the micro-benchmarks.
constexpr std::int64_t kTouch = 100'000;   // Best bid; the best ask is one tick above.

/// Distance from the touch, 0 <= d < 128, with probability ~ 1/(d+1).
std::int64_t near_touch(ob::Rng& rng) {
  static const std::vector<std::uint64_t> cumulative = [] {
    std::vector<std::uint64_t> c;
    std::uint64_t total = 0;
    for (std::uint64_t d = 0; d < 128; ++d) {
      total += (std::uint64_t{1} << 20U) / (d + 1);
      c.push_back(total);
    }
    return c;
  }();
  const std::uint64_t r = rng.below(cumulative.back());
  return std::ranges::upper_bound(cumulative, r) - cumulative.begin();
}

/// Non-crossing orders near the touch, on both sides.
std::vector<NewOrder> passive_orders(std::size_t count, std::uint64_t seed) {
  ob::Rng rng(seed);
  std::vector<NewOrder> orders;
  orders.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const Side side = rng.chance(1, 2) ? Side::buy : Side::sell;
    const std::int64_t d = near_touch(rng);
    const std::int64_t price = side == Side::buy ? kTouch - d : kTouch + 1 + d;
    orders.push_back(NewOrder{SymbolId{0}, OrderId{i + 1}, OwnerId{0}, side, OrderType::limit,
                              Price{price},
                              Qty{100 * (1 + static_cast<std::int64_t>(rng.below(10)))}});
  }
  return orders;
}

template <class Book>
std::unique_ptr<Book> fresh_book() {
  ob::BookConfig config;
  config.initial_capacity = 2 * kBatch;
  return std::make_unique<Book>(config);
}

/// An empty book that has already held `orders`: the memory it reserved has
/// been written once, so first-touch page faults do not land in timed code.
/// Orders are cancelled in arrival order, which keeps the reference book's
/// queue search short.
template <class Book>
std::unique_ptr<Book> warm_book(const std::vector<NewOrder>& orders) {
  std::unique_ptr<Book> book = fresh_book<Book>();
  ob::NullSink sink;
  for (const NewOrder& order : orders) {
    book->submit(order, sink);
  }
  for (const NewOrder& order : orders) {
    book->cancel(ob::CancelOrder{order.symbol, order.id}, sink);
  }
  return book;
}

// --- Micro-benchmarks ---------------------------------------------------------

/// Adds passive orders to a (warm) book that grows from empty to kBatch orders.
template <class Book>
void BM_Add(benchmark::State& state) {
  const std::vector<NewOrder> orders = passive_orders(kBatch, 1);
  std::unique_ptr<Book> book = warm_book<Book>(orders);
  CountingSink sink;
  std::size_t next = 0;
  for ([[maybe_unused]] const auto _ : state) {
    if (next == orders.size()) {
      state.PauseTiming();
      book = warm_book<Book>(orders);  // The full book is destroyed while paused.
      next = 0;
      state.ResumeTiming();
    }
    book->submit(orders[next++], sink);
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations());
}

/// Cancels resting orders in random order from a book of kBatch orders.
template <class Book>
void BM_Cancel(benchmark::State& state) {
  const std::vector<NewOrder> orders = passive_orders(kBatch, 2);
  std::vector<ob::CancelOrder> cancels;
  cancels.reserve(orders.size());
  for (const NewOrder& order : orders) {
    cancels.push_back(ob::CancelOrder{SymbolId{0}, order.id});
  }
  ob::Rng rng(3);
  for (std::size_t i = cancels.size() - 1; i > 0; --i) {  // Fisher-Yates
    std::swap(cancels[i], cancels[rng.below(i + 1)]);
  }
  std::unique_ptr<Book> book;
  CountingSink sink;
  std::size_t next = cancels.size();
  for ([[maybe_unused]] const auto _ : state) {
    if (next == cancels.size()) {
      state.PauseTiming();
      book = fresh_book<Book>();
      for (const NewOrder& order : orders) {
        book->submit(order, sink);
      }
      next = 0;
      state.ResumeTiming();
    }
    book->cancel(cancels[next++], sink);
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations());
}

/// Market orders of 100 to 500 lots against a deep ask side of 100-lot
/// orders: each iteration fills one to five makers.
template <class Book>
void BM_Match(benchmark::State& state) {
  ob::Rng rng(4);
  std::vector<NewOrder> asks;
  asks.reserve(kBatch);
  for (std::size_t i = 0; i < kBatch; ++i) {
    asks.push_back(NewOrder{SymbolId{0}, OrderId{i + 1}, OwnerId{0}, Side::sell, OrderType::limit,
                            Price{kTouch + 1 + near_touch(rng)}, Qty{100}});
  }
  std::vector<Qty> sizes;
  sizes.reserve(4096);
  for (std::size_t i = 0; i < 4096; ++i) {
    sizes.emplace_back(100 * (1 + static_cast<std::int64_t>(rng.below(5))));
  }
  std::unique_ptr<Book> book;
  CountingSink sink;
  std::uint64_t next_id = kBatch + 1;
  std::int64_t remaining = 0;  // Lots left on the ask side.
  std::size_t next = 0;
  for ([[maybe_unused]] const auto _ : state) {
    if (remaining < std::int64_t{500} * 100) {
      state.PauseTiming();
      book = fresh_book<Book>();
      for (const NewOrder& order : asks) {
        book->submit(order, sink);
      }
      remaining = static_cast<std::int64_t>(kBatch) * 100;
      state.ResumeTiming();
    }
    const Qty qty = sizes[next++ % sizes.size()];
    book->submit(NewOrder{SymbolId{0}, OrderId{next_id++}, OwnerId{0}, Side::buy, OrderType::market,
                          Price{0}, qty},
                 sink);
    remaining -= qty.value();
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations());
}

// --- Synthetic workload ---------------------------------------------------------

enum class Flow : std::uint8_t {
  near_touch,  // obgen defaults: passive orders within 128 ticks of the touch
  wide,        // passive orders up to 20,000 ticks away, past the ladder window
};

const ob::workload::Workload& workload(Flow flow) {
  static const auto make = [](std::uint32_t max_distance) {
    ob::workload::Config config;  // obgen defaults, seed 1
    config.commands = 1'000'000;
    config.max_distance = max_distance;
    return ob::workload::generate(config);
  };
  static const ob::workload::Workload near = make(128);
  static const ob::workload::Workload wide = make(20'000);
  return flow == Flow::wide ? wide : near;
}

template <class Book>
std::unique_ptr<ob::BasicEngine<Book>> fresh_engine(const ob::workload::Workload& w) {
  ob::BookConfig config;
  config.initial_capacity = 1U << 14U;
  auto engine = std::make_unique<ob::BasicEngine<Book>>(config);
  for (const auto& name : w.symbols) {
    engine->add_symbol(name);
  }
  return engine;
}

/// Replays the workload; one iteration is one command.
template <class Book, Flow F>
void BM_Workload(benchmark::State& state) {
  const ob::workload::Workload& w = workload(F);
  auto engine = fresh_engine<Book>(w);
  CountingSink sink;
  std::size_t next = 0;
  for ([[maybe_unused]] const auto _ : state) {
    if (next == w.commands.size()) {
      state.PauseTiming();
      engine = fresh_engine<Book>(w);
      next = 0;
      state.ResumeTiming();
    }
    engine->apply(w.commands[next++], sink);
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations());
}

/// Times every command of one pass over the workload and reports latency
/// percentiles. The per-command time includes reading the clock twice.
template <class Book, Flow F>
void BM_WorkloadLatency(benchmark::State& state) {
  const ob::workload::Workload& w = workload(F);
  auto histogram = std::make_unique<ob::bench::LatencyHistogram>();
  CountingSink sink;
  for ([[maybe_unused]] const auto _ : state) {
    state.PauseTiming();
    auto engine = fresh_engine<Book>(w);
    state.ResumeTiming();
    for (const ob::Command& command : w.commands) {
      const auto start = std::chrono::steady_clock::now();
      engine->apply(command, sink);
      const auto stop = std::chrono::steady_clock::now();
      histogram->record(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()));
    }
    state.PauseTiming();
    engine.reset();  // Teardown is not part of the measurement.
    state.ResumeTiming();
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(w.commands.size()));
  const auto counter = [&](std::uint64_t numerator, std::uint64_t denominator) {
    return benchmark::Counter(static_cast<double>(histogram->quantile(numerator, denominator)));
  };
  state.counters["p50_ns"] = counter(1, 2);
  state.counters["p99_ns"] = counter(99, 100);
  state.counters["p99.9_ns"] = counter(999, 1000);
  state.counters["max_ns"] = benchmark::Counter(static_cast<double>(histogram->max()));
}

// --- Known worst cases of the ladder ------------------------------------------

constexpr std::int64_t kAskLevels = 3000;  // One-lot asks at kTouch + 1, + 2, ...

/// A one-sided book of kAskLevels consecutive ask levels inside the ladder
/// window, and one order submitted `state.range(0)` ticks better than the
/// best ask and cancelled again, alternately: one iteration is one request.
/// Past W/8 the submit moves the window down, evicting the far end of the
/// book into the overflow; the move back up waits until as many requests
/// have passed as levels moved. Past about W the cancel empties the ring,
/// which forces a move back on every toggle.
template <class Book>
void BM_WindowToggle(benchmark::State& state) {
  const std::int64_t distance = state.range(0);
  std::unique_ptr<Book> book = fresh_book<Book>();
  CountingSink sink;
  for (std::int64_t i = 0; i < kAskLevels; ++i) {
    book->submit(NewOrder{SymbolId{0}, OrderId{static_cast<std::uint64_t>(i) + 1}, OwnerId{0},
                          Side::sell, OrderType::limit, Price{kTouch + 1 + i}, Qty{1}},
                 sink);
  }
  const NewOrder toggled{SymbolId{0},      OrderId{1'000'000},           OwnerId{0}, Side::sell,
                         OrderType::limit, Price{kTouch + 1 - distance}, Qty{1}};
  bool resting = false;
  for ([[maybe_unused]] const auto _ : state) {
    if (resting) {
      book->cancel(ob::CancelOrder{SymbolId{0}, toggled.id}, sink);
    } else {
      book->submit(toggled, sink);
    }
    resting = !resting;
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations());
}

constexpr std::int64_t kFarLevels = 100'000;  // Asks 10,000 ticks and more past the touch.

/// One ask at the touch and kFarLevels ask levels far outside the ladder
/// window (in its sorted overflow, every second tick), then one order added
/// on a new far level and cancelled again, alternately: one iteration is one
/// request. Each add or cancel inserts into or erases from the overflow
/// vector at a random position.
template <class Book>
void BM_DeepOverflow(benchmark::State& state) {
  ob::BookConfig config;
  config.initial_capacity = 2 * kFarLevels;
  auto book = std::make_unique<Book>(config);
  CountingSink sink;
  const auto ask = [](std::uint64_t id, std::int64_t price) {
    return NewOrder{SymbolId{0},      OrderId{id},  OwnerId{0}, Side::sell,
                    OrderType::limit, Price{price}, Qty{1}};
  };
  book->submit(ask(1, kTouch + 1), sink);
  // Farthest first, so that every insert lands at the back of the overflow.
  for (std::int64_t i = kFarLevels - 1; i >= 0; --i) {
    book->submit(ask(static_cast<std::uint64_t>(i) + 2, kTouch + 10'000 + (2 * i)), sink);
  }
  ob::Rng rng(5);
  std::vector<NewOrder> probes;
  for (std::uint64_t i = 0; i < 4096; ++i) {
    const auto level = static_cast<std::int64_t>(rng.below(kFarLevels));
    probes.push_back(ask(10'000'000 + i, kTouch + 10'000 + (2 * level) + 1));  // A free tick.
  }
  std::size_t next = 0;
  bool resting = false;
  for ([[maybe_unused]] const auto _ : state) {
    if (resting) {
      book->cancel(ob::CancelOrder{SymbolId{0}, probes[next].id}, sink);
      next = (next + 1) % probes.size();
    } else {
      book->submit(probes[next], sink);
    }
    resting = !resting;
  }
  benchmark::DoNotOptimize(sink.events);
  state.SetItemsProcessed(state.iterations());
}

/// Smallest non-zero step of steady_clock and the cost of reading it, so
/// that latency percentiles can be read in context.
void BM_ClockResolution(benchmark::State& state) {
  std::int64_t smallest = INT64_MAX;
  for ([[maybe_unused]] const auto _ : state) {
    const auto start = std::chrono::steady_clock::now();
    auto stop = std::chrono::steady_clock::now();
    while (stop == start) {
      stop = std::chrono::steady_clock::now();
    }
    smallest = std::min<std::int64_t>(
        smallest, std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
  }
  state.counters["resolution_ns"] = benchmark::Counter(static_cast<double>(smallest));
}

using Ladder = ob::OrderBook;
using Map = ob::MapOrderBook;
using Reference = ob::ReferenceBook;

BENCHMARK(BM_Add<Ladder>)->Name("Add/Ladder");
BENCHMARK(BM_Add<Map>)->Name("Add/Map");
BENCHMARK(BM_Add<Reference>)->Name("Add/Reference");
BENCHMARK(BM_Cancel<Ladder>)->Name("Cancel/Ladder");
BENCHMARK(BM_Cancel<Map>)->Name("Cancel/Map");
BENCHMARK(BM_Cancel<Reference>)->Name("Cancel/Reference");
BENCHMARK(BM_Match<Ladder>)->Name("Match/Ladder");
BENCHMARK(BM_Match<Map>)->Name("Match/Map");
BENCHMARK(BM_Match<Reference>)->Name("Match/Reference");
BENCHMARK(BM_Workload<Ladder, Flow::near_touch>)->Name("Workload/Ladder");
BENCHMARK(BM_Workload<Map, Flow::near_touch>)->Name("Workload/Map");
BENCHMARK(BM_Workload<Reference, Flow::near_touch>)->Name("Workload/Reference");
BENCHMARK(BM_Workload<Ladder, Flow::wide>)->Name("WorkloadWide/Ladder");
BENCHMARK(BM_Workload<Map, Flow::wide>)->Name("WorkloadWide/Map");
BENCHMARK(BM_Workload<Reference, Flow::wide>)->Name("WorkloadWide/Reference");
BENCHMARK(BM_WorkloadLatency<Ladder, Flow::near_touch>)
    ->Name("Latency/Ladder")
    ->Iterations(3)
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_WorkloadLatency<Map, Flow::near_touch>)
    ->Name("Latency/Map")
    ->Iterations(3)
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_WorkloadLatency<Reference, Flow::near_touch>)
    ->Name("Latency/Reference")
    ->Iterations(3)
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_WindowToggle<Ladder>)
    ->Name("WindowToggle/Ladder")
    ->Arg(600)
    ->Arg(1600)
    ->Arg(3000)
    ->Arg(5000);
BENCHMARK(BM_WindowToggle<Map>)
    ->Name("WindowToggle/Map")
    ->Arg(600)
    ->Arg(1600)
    ->Arg(3000)
    ->Arg(5000);
BENCHMARK(BM_DeepOverflow<Ladder>)->Name("DeepOverflow/Ladder");
BENCHMARK(BM_DeepOverflow<Map>)->Name("DeepOverflow/Map");
BENCHMARK(BM_ClockResolution)->Name("ClockResolution");

}  // namespace

BENCHMARK_MAIN();
