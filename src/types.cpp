// SPDX-License-Identifier: MIT
#include <orderbook/types.hpp>

namespace ob {

std::string_view to_string(Side side) noexcept {
  switch (side) {
    case Side::buy:
      return "buy";
    case Side::sell:
      return "sell";
  }
  return "?";
}

std::string_view to_string(OrderType type) noexcept {
  switch (type) {
    case OrderType::limit:
      return "limit";
    case OrderType::market:
      return "market";
    case OrderType::ioc:
      return "ioc";
    case OrderType::fok:
      return "fok";
    case OrderType::post_only:
      return "post_only";
  }
  return "?";
}

std::string_view to_string(StpPolicy policy) noexcept {
  switch (policy) {
    case StpPolicy::none:
      return "none";
    case StpPolicy::cancel_newest:
      return "cancel_newest";
    case StpPolicy::cancel_oldest:
      return "cancel_oldest";
  }
  return "?";
}

std::string_view to_string(Request request) noexcept {
  switch (request) {
    case Request::new_order:
      return "new";
    case Request::cancel:
      return "cancel";
    case Request::modify:
      return "modify";
  }
  return "?";
}

std::string_view to_string(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::invalid_order_id:
      return "invalid_order_id";
    case RejectReason::invalid_quantity:
      return "invalid_quantity";
    case RejectReason::invalid_price:
      return "invalid_price";
    case RejectReason::duplicate_order_id:
      return "duplicate_order_id";
    case RejectReason::unknown_order:
      return "unknown_order";
    case RejectReason::unknown_symbol:
      return "unknown_symbol";
    case RejectReason::post_only_would_cross:
      return "post_only_would_cross";
    case RejectReason::book_full:
      return "book_full";
  }
  return "?";
}

std::string_view to_string(CancelReason reason) noexcept {
  switch (reason) {
    case CancelReason::requested:
      return "requested";
    case CancelReason::ioc_remainder:
      return "ioc_remainder";
    case CancelReason::fok_unfillable:
      return "fok_unfillable";
    case CancelReason::self_trade:
      return "self_trade";
  }
  return "?";
}

}  // namespace ob
