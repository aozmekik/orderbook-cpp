// SPDX-License-Identifier: MIT
// Helpers shared by the tests: request builders, event rendering and a
// harness that runs a request against a book and returns its events as text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/events.hpp>
#include <orderbook/format.hpp>
#include <orderbook/types.hpp>

namespace ob {

// Lets GoogleTest print events readably in failure messages.
inline void PrintTo(const Event& event,
                    std::ostream* os) {  // NOLINT(readability-identifier-naming)
  std::string line;
  append_event(line, event, {});
  if (!line.empty() && line.back() == '\n') {
    line.pop_back();
  }
  *os << line;
}

inline void PrintTo(const LevelView& level,
                    std::ostream* os) {  // NOLINT(readability-identifier-naming)
  *os << "{price=" << level.price.value() << " qty=" << level.qty.value()
      << " orders=" << level.count << "}";
}

inline void PrintTo(const OrderView& order,
                    std::ostream* os) {  // NOLINT(readability-identifier-naming)
  *os << "{id=" << to_underlying(order.id) << " owner=" << to_underlying(order.owner)
      << " side=" << to_string(order.side) << " type=" << to_string(order.type)
      << " price=" << order.price.value() << " qty=" << order.qty.value() << "}";
}

}  // namespace ob

namespace ob::test {

inline constexpr Side kBuy = Side::buy;
inline constexpr Side kSell = Side::sell;

/// Symbol names used when rendering events in tests.
inline const std::vector<std::string>& symbols() {
  static const std::vector<std::string> names{"X", "Y", "Z"};
  return names;
}

inline NewOrder order(std::uint64_t id, Side side, OrderType type, std::int64_t price,
                      std::int64_t qty, std::uint32_t owner = 0) {
  return NewOrder{SymbolId{0}, OrderId{id}, OwnerId{owner}, side, type, Price{price}, Qty{qty}};
}

inline NewOrder limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty,
                      std::uint32_t owner = 0) {
  return order(id, side, OrderType::limit, price, qty, owner);
}

inline NewOrder market(std::uint64_t id, Side side, std::int64_t qty, std::uint32_t owner = 0) {
  return order(id, side, OrderType::market, 0, qty, owner);
}

inline CancelOrder cancel(std::uint64_t id) {
  return CancelOrder{SymbolId{0}, OrderId{id}};
}

inline ModifyOrder modify(std::uint64_t id, std::int64_t price, std::int64_t qty) {
  return ModifyOrder{SymbolId{0}, OrderId{id}, Price{price}, Qty{qty}};
}

/// One line per event, without trailing newlines.
inline std::vector<std::string> render(std::span<const Event> events) {
  std::vector<std::string> lines;
  for (const Event& event : events) {
    std::string line;
    append_event(line, event, symbols());
    line.pop_back();
    lines.push_back(std::move(line));
  }
  return lines;
}

/// Runs requests against a book and returns the events they produced as
/// text; checks the book's invariants after every request.
template <OrderBookLike Book>
class Harness {
 public:
  explicit Harness(const BookConfig& config = {}) : book(config) {}

  std::vector<std::string> submit(const NewOrder& request) {
    VectorSink sink;
    book.submit(request, sink);
    return finish(sink);
  }

  std::vector<std::string> cancel(const CancelOrder& request) {
    VectorSink sink;
    book.cancel(request, sink);
    return finish(sink);
  }

  std::vector<std::string> modify(const ModifyOrder& request) {
    VectorSink sink;
    book.modify(request, sink);
    return finish(sink);
  }

  /// Ids at `price` on `side`, in queue order.
  [[nodiscard]] std::vector<std::uint64_t> queue(Side side, std::int64_t price) const {
    std::vector<std::uint64_t> ids;
    book.for_each_order(side, Price{price},
                        [&](const OrderView& view) { ids.push_back(to_underlying(view.id)); });
    return ids;
  }

  /// Up to `levels` levels of `side`, best first.
  [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t levels = 64) const {
    std::vector<LevelView> out(levels);
    out.resize(book.depth(side, out));
    return out;
  }

  /// The last invariant violation seen, if any.
  std::optional<std::string> violation;
  Book book;

 private:
  std::vector<std::string> finish(const VectorSink& sink) {
    if (std::optional<std::string> problem = book.check_invariants()) {
      violation = std::move(problem);
    }
    return render(sink.events);
  }
};

/// The resting order `id` of `book`; records a test failure if it is not live.
template <OrderBookLike Book>
OrderView live_order(const Book& book, std::uint64_t id) {
  const std::optional<OrderView> order = book.find(OrderId{id});
  if (!order.has_value()) {
    ADD_FAILURE() << "order " << id << " is not resting";
    return OrderView{};
  }
  return *order;
}

/// Shorthand for building expected event lines.
using Lines = std::vector<std::string>;

}  // namespace ob::test
