// SPDX-License-Identifier: MIT
// Differential testing: the optimised book (with a narrow and a wide ladder)
// and the std::map baseline must emit exactly the same events as the
// reference book for long random request streams, and every book must pass
// its invariant checks after every request.
//
// The stream is adversarial rather than realistic: crossing prices, all order
// types, a handful of owners (so self-trade prevention fires), duplicate and
// unknown ids, invalid fields, price jumps far outside the ladder window and
// prices at the ends of the valid range.
//
// OB_DIFF_OPS overrides the number of requests per configuration.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/events.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/random.hpp>
#include <orderbook/reference_book.hpp>
#include <orderbook/types.hpp>

#include "test_support.hpp"

namespace ob {
namespace {

// Debug builds re-check every invariant inside each book after every
// request, which costs O(book size), so they run fewer requests: over the 24
// configurations, 2.4 million requests in debug builds and 6 million in
// release builds.
constexpr bool kBooksSelfCheck = OB_ENABLE_INVARIANT_CHECKS != 0;
constexpr std::uint64_t kDefaultOperations = kBooksSelfCheck ? 100'000 : 250'000;

std::uint64_t operations_per_run() {
  if (const char* value = std::getenv("OB_DIFF_OPS")) {  // NOLINT(concurrency-mt-unsafe)
    return std::strtoull(value, nullptr, 10);
  }
  return kDefaultOperations;
}

/// Live order ids with O(1) insert, erase and uniform pick.
class LiveIds {
 public:
  void add(OrderId id) {
    if (positions_.emplace(id, ids_.size()).second) {
      ids_.push_back(id);
    }
  }

  void remove(OrderId id) {
    const auto it = positions_.find(id);
    if (it == positions_.end()) {
      return;
    }
    const std::size_t position = it->second;
    positions_.erase(it);
    const OrderId last = ids_.back();
    ids_.pop_back();
    if (last != id) {
      ids_[position] = last;
      positions_[last] = position;
    }
  }

  [[nodiscard]] OrderId pick(Rng& rng) const { return ids_[rng.below(ids_.size())]; }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

 private:
  std::vector<OrderId> ids_;
  std::unordered_map<OrderId, std::size_t> positions_;
};

/// Generates the adversarial request stream. It follows the reference book's
/// events to aim most cancels and modifies at live orders; the cancel rate
/// grows with the book so that its size stays in the hundreds.
class RandomFlow {
 public:
  RandomFlow(std::uint64_t seed, std::uint32_t window) : rng_(seed), window_(window) {}

  Command next(const ReferenceBook& book) {
    drift();
    const std::uint64_t kind = rng_.below(100);
    const std::uint64_t live = live_.size();
    const std::uint64_t cancel_share = 35 * std::min<std::uint64_t>(live, 400) / 400;
    if (kind < 20 && live > 0) {
      return modify(book);
    }
    if (kind < 20 + cancel_share) {
      return CancelOrder{SymbolId{0}, pick_id()};
    }
    return new_order();
  }

  /// Updates the live set after `command` produced `events`.
  void observe(const Command& command, const std::vector<Event>& events,
               const ReferenceBook& book) {
    for (const Event& event : events) {
      if (const auto* fill = std::get_if<Fill>(&event);
          fill != nullptr && fill->maker_remaining == Qty{0}) {
        live_.remove(fill->maker_id);
      } else if (const auto* cancelled = std::get_if<Cancelled>(&event)) {
        live_.remove(cancelled->id);
      }
    }
    const OrderId id = std::visit([](const auto& c) { return c.id; }, command);
    if (book.find(id)) {
      live_.add(id);
    } else {
      live_.remove(id);
    }
  }

  [[nodiscard]] std::size_t live() const noexcept { return live_.size(); }

 private:
  void drift() {
    const std::uint64_t r = rng_.below(1000);
    if (r < 10) {
      center_ += rng_.between(-3, 3);
    } else if (r < 12) {
      center_ += rng_.between(-20, 20) * static_cast<std::int64_t>(window_);
    }
  }

  std::int64_t price() {
    const std::uint64_t r = rng_.below(10'000);
    if (r < 5) {
      return kMinPrice.value() + rng_.between(0, 3);
    }
    if (r < 10) {
      return kMaxPrice.value() - rng_.between(0, 3);
    }
    if (r < 30) {
      return kMaxPrice.value() + 1;  // Invalid.
    }
    if (r < 330) {
      return center_ + (rng_.between(-3, 3) * static_cast<std::int64_t>(window_));
    }
    return center_ + rng_.between(-12, 12);
  }

  std::int64_t qty() {
    const std::uint64_t r = rng_.below(1000);
    if (r < 10) {
      return rng_.chance(1, 2) ? 0 : -1;
    }
    if (r < 15) {
      return kMaxOrderQty.value() + 1;
    }
    if (r < 25) {
      return kMaxOrderQty.value() - rng_.between(0, 2);
    }
    if (r < 175) {
      return rng_.between(1, 100);
    }
    return rng_.between(1, 10);
  }

  OrderId pick_id() {
    if (rng_.chance(1, 10) || live_.size() == 0) {
      return OrderId{rng_.below(4) == 0 ? 0 : next_id_ + rng_.below(1000)};  // Unknown.
    }
    return live_.pick(rng_);
  }

  NewOrder new_order() {
    NewOrder order;
    const std::uint64_t id_kind = rng_.below(100);
    if (id_kind < 3 && live_.size() > 0) {
      order.id = live_.pick(rng_);  // Duplicate of a live order.
    } else if (id_kind < 6 && next_id_ > 1) {
      order.id = OrderId{1 + rng_.below(next_id_ - 1)};  // Usually a finished order: reuse.
    } else if (id_kind < 7) {
      order.id = OrderId{0};
    } else {
      order.id = OrderId{next_id_++};
    }
    order.owner = OwnerId{static_cast<std::uint32_t>(rng_.below(4))};
    order.side = rng_.chance(1, 2) ? Side::buy : Side::sell;
    order.type = type_for(rng_.below(100));
    order.price = Price{price()};
    order.qty = Qty{qty()};
    return order;
  }

  static OrderType type_for(std::uint64_t roll) noexcept {
    if (roll < 45) {
      return OrderType::limit;
    }
    if (roll < 57) {
      return OrderType::post_only;
    }
    if (roll < 72) {
      return OrderType::ioc;
    }
    return roll < 85 ? OrderType::fok : OrderType::market;
  }

  ModifyOrder modify(const ReferenceBook& book) {
    ModifyOrder request{SymbolId{0}, pick_id(), Price{price()}, Qty{qty()}};
    if (const std::optional<OrderView> live = book.find(request.id)) {
      const std::uint64_t r = rng_.below(100);
      const std::int64_t p = live->price.value();
      const std::int64_t q = live->qty.value();
      if (r < 30) {
        request.price = live->price;  // Size change only.
        request.qty = Qty{rng_.between(1, q + 5)};
      } else if (r < 45) {
        request.price = live->price;
        request.qty = live->qty;  // No-op.
      } else if (r < 85 && p > kMinPrice.value() + 5 && p < kMaxPrice.value() - 5) {
        request.price = Price{p + rng_.between(-5, 5)};
        request.qty = Qty{rng_.between(1, q + 5)};
      } else if (const std::optional<LevelView> other = book.best(opposite(live->side))) {
        request.price = other->price;  // Crossing.
      }
    }
    return request;
  }

  Rng rng_;
  std::uint32_t window_;
  std::int64_t center_ = 0;
  std::uint64_t next_id_ = 1;
  LiveIds live_;
};

std::string describe(const Command& command) {
  std::string out;
  append_csv(out, to_csv_record(command, "X"));
  return out;
}

template <class Book>
void apply(Book& book, const Command& command, VectorSink& sink) {
  sink.events.clear();
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    book.submit(*order, sink);
  } else if (const auto* cancel = std::get_if<CancelOrder>(&command)) {
    book.cancel(*cancel, sink);
  } else {
    book.modify(std::get<ModifyOrder>(command), sink);
  }
}

template <class Book>
std::vector<LevelView> full_depth(const Book& book, Side side) {
  std::vector<LevelView> levels(book.level_count(side));
  levels.resize(book.depth(side, levels));
  return levels;
}

struct DiffParams {
  std::uint64_t seed;
  StpPolicy stp;
  std::uint32_t ladder_ticks;
};

void PrintTo(const DiffParams& p, std::ostream* os) {  // NOLINT(readability-identifier-naming)
  *os << "seed " << p.seed << ", stp " << to_string(p.stp) << ", ladder " << p.ladder_ticks;
}

class DifferentialTest : public ::testing::TestWithParam<DiffParams> {};

TEST_P(DifferentialTest, OptimisedBooksAgreeWithTheReference) {
  const DiffParams params = GetParam();
  BookConfig config;
  config.stp = params.stp;
  config.ladder_ticks = params.ladder_ticks;
  config.initial_capacity = 64;  // Small, so that pools and the id map grow during the run.
  OrderBook ladder(config);
  MapOrderBook map(config);
  ReferenceBook reference(config);
  RandomFlow flow(params.seed, params.ladder_ticks);
  VectorSink expected;
  VectorSink ladder_events;
  VectorSink map_events;
  std::uint64_t fills = 0;
  std::size_t max_live = 0;

  const std::uint64_t operations = operations_per_run();
  for (std::uint64_t op = 0; op < operations; ++op) {
    const Command command = flow.next(reference);
    apply(reference, command, expected);
    apply(ladder, command, ladder_events);
    apply(map, command, map_events);
    flow.observe(command, expected.events, reference);
    max_live = std::max(max_live, flow.live());
    ASSERT_EQ(ladder_events.events, expected.events)
        << "ladder, op " << op << ": " << describe(command);
    ASSERT_EQ(map_events.events, expected.events) << "map, op " << op << ": " << describe(command);
    for (const Event& event : expected.events) {
      fills += std::holds_alternative<Fill>(event) ? 1U : 0U;
    }
    if constexpr (!kBooksSelfCheck) {
      ASSERT_EQ(ladder.check_invariants(), std::nullopt) << "op " << op;
      ASSERT_EQ(map.check_invariants(), std::nullopt) << "op " << op;
    }
    ASSERT_EQ(reference.check_invariants(), std::nullopt) << "op " << op;
    if (op % 1024 == 0 || op + 1 == operations) {
      for (const Side side : {Side::buy, Side::sell}) {
        const std::vector<LevelView> levels = full_depth(reference, side);
        ASSERT_EQ(full_depth(ladder, side), levels) << "op " << op;
        ASSERT_EQ(full_depth(map, side), levels) << "op " << op;
      }
      ASSERT_EQ(ladder.order_count(), reference.order_count());
      ASSERT_EQ(map.order_count(), reference.order_count());
    }
  }
  // The stream must actually exercise matching, not just pass trivially.
  EXPECT_GT(fills, operations / 20);
  EXPECT_GT(max_live, 40U);
  RecordProperty("fills", std::to_string(fills));
  RecordProperty("max_live_orders", std::to_string(max_live));
}

std::vector<DiffParams> all_params() {
  std::vector<DiffParams> params;
  for (const StpPolicy stp :
       {StpPolicy::none, StpPolicy::cancel_newest, StpPolicy::cancel_oldest}) {
    for (const std::uint32_t ladder : {64U, 4096U}) {
      for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        params.push_back(DiffParams{(seed * 7919) + ladder, stp, ladder});
      }
    }
  }
  return params;
}

INSTANTIATE_TEST_SUITE_P(RandomStreams, DifferentialTest, ::testing::ValuesIn(all_params()));

}  // namespace
}  // namespace ob
