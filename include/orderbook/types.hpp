// SPDX-License-Identifier: MIT
/// @file
/// Vocabulary types shared by every part of the library: strongly typed
/// prices, quantities and identifiers, and the enumerations that describe
/// orders and events.

#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace ob {

/// A signed integer that only combines with values of the same `Tag`.
///
/// Prices and quantities are both 64-bit integers; giving them distinct types
/// turns "passed a quantity where a price was expected" into a compile error.
template <class Tag, std::signed_integral Rep>
class StrongInt {
 public:
  /// The underlying integer type.
  using rep = Rep;

  /// Zero.
  constexpr StrongInt() noexcept = default;

  /// Wraps `value`. Explicit so that a bare integer never converts silently.
  constexpr explicit StrongInt(Rep value) noexcept : value_(value) {}

  /// The underlying integer.
  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

  /// Three-way comparison with another value of the same type.
  friend constexpr auto operator<=>(const StrongInt&, const StrongInt&) noexcept = default;

  /// Sum. The caller guarantees that the result is representable.
  friend constexpr StrongInt operator+(StrongInt lhs, StrongInt rhs) noexcept {
    return StrongInt{static_cast<Rep>(lhs.value_ + rhs.value_)};
  }

  /// Difference. The caller guarantees that the result is representable.
  friend constexpr StrongInt operator-(StrongInt lhs, StrongInt rhs) noexcept {
    return StrongInt{static_cast<Rep>(lhs.value_ - rhs.value_)};
  }

  /// In-place sum.
  constexpr StrongInt& operator+=(StrongInt rhs) noexcept {
    value_ = static_cast<Rep>(value_ + rhs.value_);
    return *this;
  }

  /// In-place difference.
  constexpr StrongInt& operator-=(StrongInt rhs) noexcept {
    value_ = static_cast<Rep>(value_ - rhs.value_);
    return *this;
  }

 private:
  Rep value_{};
};

/// A price as a whole number of ticks. Matching never uses floating point.
using Price = StrongInt<struct PriceTag, std::int64_t>;

/// A quantity as a whole number of lots (shares, contracts, coins...).
using Qty = StrongInt<struct QtyTag, std::int64_t>;

/// Client-assigned order identifier, unique among the live orders of a book.
/// Zero is reserved and rejected.
enum class OrderId : std::uint64_t {};

/// Owner (account or trader) of an order, used for self-trade prevention.
/// Zero means "anonymous": self-trade prevention never applies to it.
enum class OwnerId : std::uint32_t {};

/// Dense index of a symbol inside an engine (0, 1, 2, ...).
enum class SymbolId : std::uint32_t {};

/// Converts an enumeration to its underlying integer (std::to_underlying is C++23).
template <class E>
  requires std::is_enum_v<E>
[[nodiscard]] constexpr std::underlying_type_t<E> to_underlying(E value) noexcept {
  return static_cast<std::underlying_type_t<E>>(value);
}

/// Smallest price a book accepts, in ticks. The range is symmetric and leaves
/// headroom so that negating a price or offsetting it by a ladder window can
/// never overflow. Negative prices are allowed (spreads, some futures).
inline constexpr Price kMinPrice{-(std::int64_t{1} << 62)};

/// Largest price a book accepts, in ticks.
inline constexpr Price kMaxPrice{std::int64_t{1} << 62};

/// Largest quantity of a single order. With fewer than 2^32 orders per book,
/// any sum of open quantities fits in a signed 64-bit integer.
inline constexpr Qty kMaxOrderQty{std::numeric_limits<std::int32_t>::max()};

/// Side of an order.
enum class Side : std::uint8_t {
  buy,   ///< Bid: wants to buy at or below its limit price.
  sell,  ///< Ask: wants to sell at or above its limit price.
};

/// The other side.
[[nodiscard]] constexpr Side opposite(Side side) noexcept {
  return side == Side::buy ? Side::sell : Side::buy;
}

/// What an order does when it arrives.
enum class OrderType : std::uint8_t {
  limit,      ///< Good-till-cancelled limit order: matches, then the remainder rests.
  market,     ///< Matches at any price; an unfilled remainder is cancelled.
  ioc,        ///< Immediate-or-cancel limit order: matches, the remainder is cancelled.
  fok,        ///< Fill-or-kill limit order: fills completely and at once, or not at all.
  post_only,  ///< Limit order that must not take liquidity; rejected if it would cross.
};

/// Self-trade prevention policy, applied when an incoming order would match a
/// resting order with the same non-zero owner.
enum class StpPolicy : std::uint8_t {
  none,           ///< Self-trades are allowed.
  cancel_newest,  ///< Cancel the remainder of the incoming order; the resting order stays.
  cancel_oldest,  ///< Cancel the resting order and keep matching the incoming one.
};

/// Which request a rejection answers.
enum class Request : std::uint8_t {
  new_order,  ///< A new order.
  cancel,     ///< A cancel request.
  modify,     ///< A modify (cancel/replace) request.
};

/// Why a request was rejected. A rejected request has no effect on the book.
enum class RejectReason : std::uint8_t {
  invalid_order_id,       ///< Order id zero.
  invalid_quantity,       ///< Quantity not in [1, kMaxOrderQty].
  invalid_price,          ///< Limit price not in [kMinPrice, kMaxPrice].
  duplicate_order_id,     ///< A live order of this book already uses the id.
  unknown_order,          ///< Cancel or modify of an id that is not live.
  unknown_symbol,         ///< The engine has no book for the symbol.
  post_only_would_cross,  ///< A post-only order (or its new price) would take liquidity.
  book_full,              ///< The book already holds `BookConfig::max_orders` resting orders.
};

/// Why an accepted order (or part of it) was cancelled.
enum class CancelReason : std::uint8_t {
  requested,       ///< A cancel request.
  ioc_remainder,   ///< The unfilled part of a market or IOC order.
  fok_unfillable,  ///< A fill-or-kill order that could not fill completely.
  self_trade,      ///< Self-trade prevention.
};

/// Stable lower-case name of a side ("buy", "sell").
[[nodiscard]] std::string_view to_string(Side side) noexcept;
/// Stable lower-case name of an order type ("limit", "market", "ioc", "fok", "post_only").
[[nodiscard]] std::string_view to_string(OrderType type) noexcept;
/// Stable lower-case name of a self-trade prevention policy.
[[nodiscard]] std::string_view to_string(StpPolicy policy) noexcept;
/// Stable lower-case name of a request kind ("new", "cancel", "modify").
[[nodiscard]] std::string_view to_string(Request request) noexcept;
/// Stable lower-case name of a rejection reason.
[[nodiscard]] std::string_view to_string(RejectReason reason) noexcept;
/// Stable lower-case name of a cancellation reason.
[[nodiscard]] std::string_view to_string(CancelReason reason) noexcept;

}  // namespace ob
