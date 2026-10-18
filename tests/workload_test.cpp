// SPDX-License-Identifier: MIT
// The synthetic workload generator, and determinism of replays across runs
// and across book implementations.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/format.hpp>
#include <orderbook/types.hpp>
#include <orderbook/workload.hpp>

namespace ob {
namespace {

workload::Config small_config(std::uint64_t seed) {
  workload::Config config;
  config.seed = seed;
  config.commands = 50'000;
  config.symbols = 3;
  config.target_orders = 500;
  return config;
}

std::string to_csv(const workload::Workload& w) {
  std::string out;
  for (const Command& command : w.commands) {
    const auto symbol = static_cast<std::size_t>(to_underlying(symbol_of(command)));
    append_csv(out, to_csv_record(command, w.symbols[symbol]));
  }
  return out;
}

/// FNV-1a, to pin generated output without storing it.
std::uint64_t fingerprint(std::string_view text) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

/// Everything an engine prints for a workload: all events, then snapshots.
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
  for (std::size_t i = 0; i < engine.symbol_count(); ++i) {
    append_snapshot(out, w.symbols[i], *engine.find_book(static_cast<SymbolId>(i)), 1000);
  }
  return out;
}

TEST(WorkloadTest, IsDeterministicForASeed) {
  const workload::Workload a = workload::generate(small_config(5));
  const workload::Workload b = workload::generate(small_config(5));
  const workload::Workload c = workload::generate(small_config(6));
  EXPECT_EQ(a.commands, b.commands);
  EXPECT_EQ(a.symbols, (std::vector<std::string>{"SYM1", "SYM2", "SYM3"}));
  EXPECT_NE(a.commands, c.commands);
  ASSERT_EQ(a.commands.size(), 50'000U);
}

TEST(WorkloadTest, OutputIsPinned) {
  // Benchmarks and examples quote results for these streams; a change to the
  // generator that alters them must be deliberate.
  workload::Config config;
  config.commands = 20'000;
  EXPECT_EQ(fingerprint(to_csv(workload::generate(config))), 0xeba7bd86b51e87f2ULL);
}

TEST(WorkloadTest, CancelsAndModifiesOnlyTargetLiveOrders) {
  const workload::Workload w = workload::generate(small_config(9));
  Engine engine;
  for (const std::string& name : w.symbols) {
    engine.add_symbol(name);
  }
  std::uint64_t unknown = 0;
  auto sink = [&](const auto& event) {
    if constexpr (std::is_same_v<std::decay_t<decltype(event)>, Rejected>) {
      unknown += event.reason == RejectReason::unknown_order ? 1 : 0;
    }
  };
  for (const Command& command : w.commands) {
    engine.apply(command, sink);
  }
  EXPECT_EQ(unknown, 0U);
}

TEST(WorkloadTest, DefaultMixIsCancelHeavyAndNearTheTouch) {
  workload::Config config;  // Defaults, except a smaller book so that debug builds stay fast.
  config.commands = 100'000;
  config.target_orders = 500;
  const workload::Workload w = workload::generate(config);
  const workload::Stats& s = w.stats;
  EXPECT_EQ(s.new_orders + s.cancels + s.modifies, config.commands);
  const std::uint64_t ended = s.ended_cancelled + s.ended_filled;
  ASSERT_GT(ended, 0U);
  // About nine in ten orders that leave the book are cancelled.
  EXPECT_GE(s.ended_cancelled * 100, ended * 85);
  EXPECT_LE(s.ended_cancelled * 100, ended * 95);
  // The book size stays near the target instead of growing without bound.
  EXPECT_LT(s.live_at_end, 2U * config.target_orders);
  EXPECT_GT(s.fills, 0U);

  // Most passive orders land within a few ticks of the touch.
  Engine engine;
  engine.add_symbol(w.symbols[0]);
  std::uint64_t near = 0;
  std::uint64_t passive = 0;
  NullSink sink;
  for (const Command& command : w.commands) {
    if (const auto* order = std::get_if<NewOrder>(&command);
        order != nullptr && order->type == OrderType::limit) {
      const auto& book = *engine.find_book(SymbolId{0});
      const auto touch = order->side == Side::buy ? book.best_bid() : book.best_ask();
      if (touch) {
        ++passive;
        const std::int64_t distance = order->side == Side::buy
                                          ? touch->price.value() - order->price.value()
                                          : order->price.value() - touch->price.value();
        near += distance <= 10 ? 1 : 0;
      }
    }
    engine.apply(command, sink);
  }
  EXPECT_GT(near * 100, passive * 50);
}

TEST(WorkloadTest, RejectsConfigsWhosePricesWouldLeaveTheValidRange) {
  workload::Config config;
  EXPECT_EQ(workload::config_error(config), std::nullopt);
  const std::int64_t limit = workload::max_start_price(config.max_distance);
  EXPECT_LT(limit, kMaxPrice.value());
  config.start_price = limit;
  EXPECT_EQ(workload::config_error(config), std::nullopt);
  config.start_price = -limit;
  EXPECT_EQ(workload::config_error(config), std::nullopt);
  for (const std::int64_t bad : {limit + 1, -limit - 1, std::numeric_limits<std::int64_t>::max(),
                                 std::numeric_limits<std::int64_t>::min()}) {
    config.start_price = bad;
    ASSERT_TRUE(workload::config_error(config).has_value()) << bad;
    EXPECT_THROW(static_cast<void>(workload::generate(config)), std::invalid_argument) << bad;
  }
}

TEST(WorkloadTest, RejectsOutOfRangeShares) {
  workload::Config config;
  config.max_distance = 0;
  EXPECT_TRUE(workload::config_error(config).has_value());
  config.max_distance = workload::kMaxDistance + 1;
  EXPECT_TRUE(workload::config_error(config).has_value());
  config = workload::Config{};
  config.post_only_percent = 101;
  EXPECT_TRUE(workload::config_error(config).has_value());
  config = workload::Config{};
  config.market_percent = 60;
  config.fok_percent = 41;
  EXPECT_TRUE(workload::config_error(config).has_value());
  config.fok_percent = 40;
  EXPECT_EQ(workload::config_error(config), std::nullopt);
}

TEST(WorkloadTest, GeneratesAtBothEdgesOfTheAcceptedRange) {
  // The generator's own arithmetic must not overflow (the sanitizer presets
  // run this test under UBSan), and its first orders must be valid. Later
  // orders may drift a few ticks past the range; books reject those.
  for (const int sign : {1, -1}) {
    workload::Config config;
    config.commands = 5'000;
    config.start_price = sign * workload::max_start_price(config.max_distance);
    const workload::Workload w = workload::generate(config);
    const auto* first = std::get_if<NewOrder>(&w.commands.front());
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(is_valid_price(first->price));
    Engine engine;
    engine.add_symbol(w.symbols.front());
    NullSink sink;
    for (const Command& command : w.commands) {
      engine.apply(command, sink);
    }
    EXPECT_EQ(engine.find_book(SymbolId{0})->check_invariants(), std::nullopt);
  }
}

TEST(DeterminismTest, ReplayingTwiceGivesIdenticalBytes) {
  const workload::Workload w = workload::generate(small_config(11));
  const std::string first = replay<Engine>(w);
  EXPECT_EQ(replay<Engine>(w), first);
  EXPECT_GT(first.size(), 1'000'000U);
}

TEST(DeterminismTest, AllBookImplementationsPrintTheSameBytes) {
  const workload::Workload w = workload::generate(small_config(12));
  const std::string expected = replay<ReferenceEngine>(w);
  EXPECT_TRUE(replay<Engine>(w) == expected);
  EXPECT_TRUE(replay<MapEngine>(w) == expected);
}

}  // namespace
}  // namespace ob
