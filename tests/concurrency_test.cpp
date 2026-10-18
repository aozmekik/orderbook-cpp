// SPDX-License-Identifier: MIT
// Books and engines share no mutable state, which is what makes sharding
// symbols across threads safe. These tests run several engines (and the
// workload generator) on separate threads at once and require each to
// produce exactly what a single-threaded run produces. Under the tsan preset,
// ThreadSanitizer also checks that no hidden shared state is raced on.

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/commands.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/format.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/workload.hpp>

namespace ob {
namespace {

constexpr bool kBooksSelfCheck = OB_ENABLE_INVARIANT_CHECKS != 0;

workload::Config config() {
  workload::Config c;
  c.commands = kBooksSelfCheck ? 20'000 : 200'000;
  c.symbols = 2;
  c.target_orders = 500;
  c.seed = 21;
  return c;
}

template <class EngineT>
std::string replay(const workload::Workload& w) {
  EngineT engine;
  for (const std::string& name : w.symbols) {
    engine.add_symbol(name);
  }
  std::string out;
  auto sink = [&](const auto& event) { append_event(out, event, engine.symbol_names()); };
  for (const Command& command : w.commands) {
    engine.apply(command, sink);
  }
  return out;
}

TEST(ConcurrencyTest, EnginesOnSeparateThreadsMatchASingleThreadedRun) {
  const workload::Workload w = workload::generate(config());
  const std::string expected = replay<Engine>(w);
  constexpr std::size_t kThreads = 6;
  std::vector<std::string> outputs(kThreads);
  {
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
      threads.emplace_back([&w, &outputs, i] {
        // Mix the implementations so that they also run side by side.
        switch (i % 3) {
          case 0:
            outputs[i] = replay<Engine>(w);
            break;
          case 1:
            outputs[i] = replay<MapEngine>(w);
            break;
          default:
            outputs[i] = replay<ReferenceEngine>(w);
            break;
        }
      });
    }
  }  // Joins.
  for (std::size_t i = 0; i < kThreads; ++i) {
    EXPECT_TRUE(outputs[i] == expected) << "thread " << i;
  }
}

TEST(ConcurrencyTest, WorkloadGenerationIsThreadIndependent) {
  const workload::Workload expected = workload::generate(config());
  constexpr std::size_t kThreads = 4;
  std::vector<workload::Workload> results(kThreads);
  {
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
      threads.emplace_back([&results, i] { results[i] = workload::generate(config()); });
    }
  }
  for (const workload::Workload& result : results) {
    EXPECT_TRUE(result.commands == expected.commands);
  }
}

}  // namespace
}  // namespace ob
