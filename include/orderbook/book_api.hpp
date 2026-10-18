// SPDX-License-Identifier: MIT
/// @file
/// Configuration, query results and request validation shared by every book
/// implementation, plus the `OrderBookLike` concept they all satisfy.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

#include <orderbook/commands.hpp>
#include <orderbook/events.hpp>
#include <orderbook/types.hpp>

namespace ob {

/// Upper bound for `BookConfig::max_orders`: order handles are 32-bit indices.
inline constexpr std::uint32_t kMaxBookOrders = std::numeric_limits<std::uint32_t>::max() - 1;

/// Settings of one book. Every field has a sensible default.
struct BookConfig {
  /// Symbol stamped on every event the book emits.
  SymbolId symbol{};
  /// Self-trade prevention policy.
  StpPolicy stp = StpPolicy::none;
  /// Maximum number of resting orders. A new limit or post-only order that
  /// arrives when the book is full is rejected with `book_full`.
  std::uint32_t max_orders = kMaxBookOrders;
  /// Orders (and price levels) to reserve memory for up front. The book grows
  /// past this like a `std::vector`, but a book that never exceeds it never
  /// allocates after construction.
  std::uint32_t initial_capacity = 1024;
  /// Width in ticks of the dense window `LadderLevels` keeps around the best
  /// price. Rounded up to a power of two, minimum 64. Ignored by other level
  /// indexes.
  std::uint32_t ladder_ticks = 4096;
};

/// Aggregate view of one price level.
struct LevelView {
  Price price;            ///< Price of the level.
  Qty qty;                ///< Total open quantity.
  std::uint32_t count{};  ///< Number of orders.

  /// Member-wise equality.
  friend bool operator==(const LevelView&, const LevelView&) = default;
};

/// View of one resting order.
struct OrderView {
  OrderId id{};                       ///< Id.
  OwnerId owner{};                    ///< Owner.
  Side side = Side::buy;              ///< Side.
  OrderType type = OrderType::limit;  ///< Type (`limit` or `post_only`: only these rest).
  Price price;                        ///< Limit price.
  Qty qty;                            ///< Open quantity.

  /// Member-wise equality.
  friend bool operator==(const OrderView&, const OrderView&) = default;
};

/// True if `qty` is a valid order quantity.
[[nodiscard]] constexpr bool is_valid_qty(Qty qty) noexcept {
  return qty > Qty{0} && qty <= kMaxOrderQty;
}

/// True if `price` is a valid limit price.
[[nodiscard]] constexpr bool is_valid_price(Price price) noexcept {
  return price >= kMinPrice && price <= kMaxPrice;
}

/// Stateless checks of a new order, in the order a book applies them:
/// order id, quantity, then price (market orders have no price).
/// Returns the reason the order must be rejected, or `std::nullopt`.
[[nodiscard]] constexpr std::optional<RejectReason> validate(const NewOrder& order) noexcept {
  if (order.id == OrderId{0}) {
    return RejectReason::invalid_order_id;
  }
  if (!is_valid_qty(order.qty)) {
    return RejectReason::invalid_quantity;
  }
  if (order.type != OrderType::market && !is_valid_price(order.price)) {
    return RejectReason::invalid_price;
  }
  return std::nullopt;
}

/// Stateless checks of a modify request (quantity, then price).
/// Returns the reason the request must be rejected, or `std::nullopt`.
[[nodiscard]] constexpr std::optional<RejectReason> validate(const ModifyOrder& request) noexcept {
  if (!is_valid_qty(request.qty)) {
    return RejectReason::invalid_quantity;
  }
  if (!is_valid_price(request.price)) {
    return RejectReason::invalid_price;
  }
  return std::nullopt;
}

/// True for order types that can rest in the book.
[[nodiscard]] constexpr bool can_rest(OrderType type) noexcept {
  return type == OrderType::limit || type == OrderType::post_only;
}

/// The interface shared by `OrderBook`, `MapOrderBook` and `ReferenceBook`.
/// `BasicEngine` works with any type that satisfies it.
template <class B>
concept OrderBookLike =
    std::constructible_from<B, const BookConfig&> &&
    requires(B& book, const B& cbook, const NewOrder& n, const CancelOrder& c, const ModifyOrder& m,
             NullSink& sink, Side side, OrderId id, std::span<LevelView> out) {
      book.submit(n, sink);
      book.cancel(c, sink);
      book.modify(m, sink);
      { cbook.best(side) } -> std::same_as<std::optional<LevelView>>;
      { cbook.depth(side, out) } -> std::same_as<std::size_t>;
      { cbook.find(id) } -> std::same_as<std::optional<OrderView>>;
      { cbook.order_count() } -> std::same_as<std::size_t>;
      { cbook.level_count(side) } -> std::same_as<std::size_t>;
      { cbook.check_invariants() } -> std::same_as<std::optional<std::string>>;
    };

}  // namespace ob
