// SPDX-License-Identifier: MIT
/// @file
/// Requests that drive a book or an engine.

#pragma once

#include <variant>

#include <orderbook/types.hpp>

namespace ob {

/// Submit a new order.
struct NewOrder {
  SymbolId symbol{};                  ///< Routes the order inside an engine; a book ignores it.
  OrderId id{};                       ///< Must be non-zero and not used by a live order.
  OwnerId owner{};                    ///< Owner for self-trade prevention; zero is anonymous.
  Side side = Side::buy;              ///< Buy or sell.
  OrderType type = OrderType::limit;  ///< Order type.
  Price price;                        ///< Limit price in ticks; ignored for market orders.
  Qty qty;                            ///< Quantity in lots, in [1, kMaxOrderQty].

  /// Member-wise equality.
  friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

/// Cancel the open quantity of a resting order.
struct CancelOrder {
  SymbolId symbol{};  ///< Routes the request inside an engine; a book ignores it.
  OrderId id{};       ///< The order to cancel.

  /// Member-wise equality.
  friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

/// Change the price and/or open quantity of a resting order.
///
/// Exchange semantics: reducing the quantity at the same price keeps the
/// order's time priority; a price change or a quantity increase moves it to
/// the back of the queue at its (new) price, and a new price that crosses the
/// spread matches like a new order.
struct ModifyOrder {
  SymbolId symbol{};  ///< Routes the request inside an engine; a book ignores it.
  OrderId id{};       ///< The order to modify.
  Price price;        ///< New limit price (equal to the current one to change only the size).
  Qty qty;            ///< New open (unfilled) quantity, in [1, kMaxOrderQty].

  /// Member-wise equality.
  friend bool operator==(const ModifyOrder&, const ModifyOrder&) = default;
};

/// Any request.
using Command = std::variant<NewOrder, CancelOrder, ModifyOrder>;

/// The symbol a command is addressed to.
[[nodiscard]] constexpr SymbolId symbol_of(const Command& command) noexcept {
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    return order->symbol;
  }
  if (const auto* cancel = std::get_if<CancelOrder>(&command)) {
    return cancel->symbol;
  }
  const auto* modify = std::get_if<ModifyOrder>(&command);
  return modify != nullptr ? modify->symbol : SymbolId{};
}

}  // namespace ob
