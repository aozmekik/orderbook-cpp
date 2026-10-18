// SPDX-License-Identifier: MIT
/// @file
/// Events a book emits while it processes a request, and the sinks that
/// receive them.
///
/// Every request produces events synchronously, in a fixed order documented
/// in docs/semantics.md. A new order produces either one `Rejected` (it
/// failed `validate()`, reused a live id, found the book full, or is a
/// post-only order that would cross) or exactly one `Accepted`; from then
/// on, its quantity is accounted for by `Fill`s, at most one `Cancelled`,
/// and whatever is left resting in the book.

#pragma once

#include <concepts>
#include <cstdint>
#include <variant>
#include <vector>

#include <orderbook/types.hpp>

namespace ob {

/// A new order passed validation. Emitted before any of its fills.
struct Accepted {
  SymbolId symbol{};                  ///< Symbol of the book.
  OrderId id{};                       ///< The order.
  OwnerId owner{};                    ///< Its owner.
  Side side = Side::buy;              ///< Its side.
  OrderType type = OrderType::limit;  ///< Its type.
  Price price;                        ///< Its limit price (zero for market orders).
  Qty qty;                            ///< Its quantity.

  /// Member-wise equality.
  friend bool operator==(const Accepted&, const Accepted&) = default;
};

/// A request was refused and had no effect.
struct Rejected {
  SymbolId symbol{};                                     ///< Symbol the request was addressed to.
  OrderId id{};                                          ///< Order id named by the request.
  Request request = Request::new_order;                  ///< Which kind of request was refused.
  RejectReason reason = RejectReason::invalid_order_id;  ///< Why.

  /// Member-wise equality.
  friend bool operator==(const Rejected&, const Rejected&) = default;
};

/// A trade between an incoming (taker) and a resting (maker) order.
struct Fill {
  SymbolId symbol{};            ///< Symbol of the book.
  OrderId taker_id{};           ///< The incoming order.
  OrderId maker_id{};           ///< The resting order.
  Side taker_side = Side::buy;  ///< Side of the taker; the maker is on the other side.
  Price price;                  ///< Trade price: always the maker's limit price.
  Qty qty;                      ///< Traded quantity.
  Qty taker_remaining;          ///< Taker quantity still open after this fill.
  Qty maker_remaining;          ///< Maker quantity still open after this fill.

  /// Member-wise equality.
  friend bool operator==(const Fill&, const Fill&) = default;
};

/// Open quantity of an accepted order was cancelled; the order is gone.
struct Cancelled {
  SymbolId symbol{};                              ///< Symbol of the book.
  OrderId id{};                                   ///< The order.
  Qty qty;                                        ///< Quantity that was cancelled.
  CancelReason reason = CancelReason::requested;  ///< Why.

  /// Member-wise equality.
  friend bool operator==(const Cancelled&, const Cancelled&) = default;
};

/// A resting order was modified. Emitted before any book change it causes.
struct Modified {
  SymbolId symbol{};           ///< Symbol of the book.
  OrderId id{};                ///< The order.
  Price price;                 ///< Its new limit price.
  Qty qty;                     ///< Its new open quantity.
  bool priority_kept = false;  ///< True if it kept its place in the queue.

  /// Member-wise equality.
  friend bool operator==(const Modified&, const Modified&) = default;
};

/// Level-2 update: the new aggregate state of one price level.
/// `qty == 0` (and `count == 0`) means the level was removed.
struct BookDelta {
  SymbolId symbol{};      ///< Symbol of the book.
  Side side = Side::buy;  ///< `buy` for a bid level, `sell` for an ask level.
  Price price;            ///< Price of the level.
  Qty qty;                ///< Total open quantity at the level.
  std::uint32_t count{};  ///< Number of orders at the level.

  /// Member-wise equality.
  friend bool operator==(const BookDelta&, const BookDelta&) = default;
};

/// Any event.
using Event = std::variant<Accepted, Rejected, Fill, Cancelled, Modified, BookDelta>;

/// A callable that receives every event type. Sinks are invoked synchronously
/// from inside the book; they must not throw and must not call back into the
/// book that is invoking them. Books take sinks by lvalue reference, so that
/// the events a sink collects cannot vanish with a temporary: declare even a
/// `NullSink` before passing it.
template <class S>
concept EventSink = std::invocable<S&, const Accepted&> && std::invocable<S&, const Rejected&> &&
                    std::invocable<S&, const Fill&> && std::invocable<S&, const Cancelled&> &&
                    std::invocable<S&, const Modified&> && std::invocable<S&, const BookDelta&>;

/// Discards every event.
struct NullSink {
  /// Ignores `event`.
  template <class E>
  constexpr void operator()(const E& /*event*/) const noexcept {}
};

/// Appends every event to a vector. Convenient for tests and tools; it
/// allocates as the vector grows, so it is not meant for the hot path.
struct VectorSink {
  std::vector<Event> events;  ///< Events in emission order.

  /// Appends `event`.
  template <class E>
  void operator()(const E& event) {
    events.emplace_back(event);
  }
};

}  // namespace ob
