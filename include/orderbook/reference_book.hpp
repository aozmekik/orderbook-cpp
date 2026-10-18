// SPDX-License-Identifier: MIT
/// @file
/// `ReferenceBook`: a deliberately simple book used as the oracle for
/// differential testing.

#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/events.hpp>
#include <orderbook/types.hpp>

namespace ob {

/// A limit order book written for obviousness rather than speed: standard
/// containers, prices compared directly (no key transforms), level totals
/// recomputed on demand, and orders found by linear search within their
/// level. It shares no matching code with `BasicOrderBook`, so agreement
/// between the two on millions of random requests is meaningful evidence
/// that both implement the semantics in docs/semantics.md.
///
/// It is a test oracle, not a hardened book: its `std::unordered_map` uses
/// `std::hash`, so clients that choose their order ids could make it slow.
class ReferenceBook {
 public:
  /// Creates an empty book. Only `symbol`, `stp` and `max_orders` matter.
  explicit ReferenceBook(const BookConfig& config = {}) : config_(config) {}

  /// Processes a new order.
  template <EventSink Sink>
  void submit(const NewOrder& order, Sink& sink) {
    if (const std::optional<RejectReason> reason = validate(order)) {
      reject(order.id, Request::new_order, *reason, sink);
      return;
    }
    if (where_.contains(order.id)) {
      reject(order.id, Request::new_order, RejectReason::duplicate_order_id, sink);
      return;
    }
    if (can_rest(order.type) && where_.size() >= config_.max_orders) {
      reject(order.id, Request::new_order, RejectReason::book_full, sink);
      return;
    }
    const Incoming incoming{order.id, order.owner, order.side, order.type == OrderType::market,
                            order.price};
    if (order.type == OrderType::post_only && crosses_now(incoming)) {
      reject(order.id, Request::new_order, RejectReason::post_only_would_cross, sink);
      return;
    }
    const Price shown_price = order.type == OrderType::market ? Price{0} : order.price;
    sink(Accepted{config_.symbol, order.id, order.owner, order.side, order.type, shown_price,
                  order.qty});
    if (order.type == OrderType::fok && fillable_qty(incoming) < order.qty) {
      sink(Cancelled{config_.symbol, order.id, order.qty, CancelReason::fok_unfillable});
      return;
    }
    const Qty left = match(incoming, order.qty, sink);
    if (left == Qty{0}) {
      return;
    }
    if (can_rest(order.type)) {
      add(order.side, order.price, Resting{order.id, order.owner, order.type, left}, sink);
    } else {
      sink(Cancelled{config_.symbol, order.id, left, CancelReason::ioc_remainder});
    }
  }

  /// Cancels a resting order.
  template <EventSink Sink>
  void cancel(const CancelOrder& request, Sink& sink) {
    const auto found = where_.find(request.id);
    if (found == where_.end()) {
      reject(request.id, Request::cancel, RejectReason::unknown_order, sink);
      return;
    }
    const Location location = found->second;
    Queue& queue = levels(location.side).at(location.price);
    const auto it = find_in(queue, request.id);
    sink(Cancelled{config_.symbol, request.id, it->qty, CancelReason::requested});
    queue.erase(it);
    where_.erase(found);
    publish_level(location.side, location.price, sink);
  }

  /// Modifies a resting order.
  template <EventSink Sink>
  void modify(const ModifyOrder& request, Sink& sink) {
    if (const std::optional<RejectReason> reason = validate(request)) {
      reject(request.id, Request::modify, *reason, sink);
      return;
    }
    const auto found = where_.find(request.id);
    if (found == where_.end()) {
      reject(request.id, Request::modify, RejectReason::unknown_order, sink);
      return;
    }
    const Location location = found->second;
    Queue& queue = levels(location.side).at(location.price);
    const auto it = find_in(queue, request.id);

    if (request.price == location.price) {
      if (request.qty <= it->qty) {
        // Smaller (or equal) size at the same price: keeps its place.
        const bool changed = request.qty != it->qty;
        it->qty = request.qty;
        sink(Modified{config_.symbol, request.id, request.price, request.qty, true});
        if (changed) {
          publish_level(location.side, location.price, sink);
        }
      } else {
        // Larger size: back of the queue.
        Resting moved = *it;
        moved.qty = request.qty;
        queue.erase(it);
        queue.push_back(moved);
        sink(Modified{config_.symbol, request.id, request.price, request.qty, false});
        publish_level(location.side, location.price, sink);
      }
      return;
    }

    // New price: behaves like cancelling and sending a new order with the same id.
    const Resting old = *it;
    const Incoming incoming{old.id, old.owner, location.side, false, request.price};
    if (old.type == OrderType::post_only && crosses_now(incoming)) {
      reject(request.id, Request::modify, RejectReason::post_only_would_cross, sink);
      return;
    }
    sink(Modified{config_.symbol, request.id, request.price, request.qty, false});
    queue.erase(it);
    where_.erase(found);
    publish_level(location.side, location.price, sink);
    const Qty left = match(incoming, request.qty, sink);
    if (left > Qty{0}) {
      add(location.side, request.price, Resting{old.id, old.owner, old.type, left}, sink);
    }
  }

  /// The best level of `side`, if any.
  [[nodiscard]] std::optional<LevelView> best(Side side) const {
    const std::optional<Price> price = best_price(side);
    if (!price) {
      return std::nullopt;
    }
    return view_of(*price, levels(side).at(*price));
  }

  /// The best bid level, if any.
  [[nodiscard]] std::optional<LevelView> best_bid() const { return best(Side::buy); }

  /// The best ask level, if any.
  [[nodiscard]] std::optional<LevelView> best_ask() const { return best(Side::sell); }

  /// Writes up to `out.size()` levels of `side`, best first; returns the count.
  [[nodiscard]] std::size_t depth(Side side, std::span<LevelView> out) const {
    std::size_t written = 0;
    const auto copy = [&](const auto& first, const auto& last) {
      for (auto it = first; it != last && written < out.size(); ++it) {
        out[written++] = view_of(it->first, it->second);
      }
    };
    if (side == Side::buy) {
      copy(bids_.rbegin(), bids_.rend());
    } else {
      copy(asks_.begin(), asks_.end());
    }
    return written;
  }

  /// The resting order with id `id`, if any.
  [[nodiscard]] std::optional<OrderView> find(OrderId id) const {
    const auto found = where_.find(id);
    if (found == where_.end()) {
      return std::nullopt;
    }
    const Location location = found->second;
    const Queue& queue = levels(location.side).at(location.price);
    const auto it = std::ranges::find(queue, id, &Resting::id);
    return OrderView{it->id, it->owner, location.side, it->type, location.price, it->qty};
  }

  /// Calls `visit(OrderView)` for the orders at `price` on `side`, in
  /// priority. Visits nothing for a price outside `[kMinPrice, kMaxPrice]`.
  template <class Visit>
  void for_each_order(Side side, Price price, Visit visit) const {
    if (!is_valid_price(price)) {
      return;
    }
    const auto level = levels(side).find(price);
    if (level == levels(side).end()) {
      return;
    }
    for (const Resting& resting : level->second) {
      visit(OrderView{resting.id, resting.owner, side, resting.type, price, resting.qty});
    }
  }

  /// Number of resting orders.
  [[nodiscard]] std::size_t order_count() const noexcept { return where_.size(); }

  /// Number of price levels on `side`.
  [[nodiscard]] std::size_t level_count(Side side) const noexcept { return levels(side).size(); }

  /// Checks that the book is not crossed, has no empty levels and that the
  /// id index agrees with the queues.
  [[nodiscard]] std::optional<std::string> check_invariants() const {
    std::size_t orders = 0;
    for (const Side side : {Side::buy, Side::sell}) {
      for (const auto& [price, queue] : levels(side)) {
        if (queue.empty()) {
          return "reference: empty level";
        }
        for (const Resting& resting : queue) {
          const auto found = where_.find(resting.id);
          if (found == where_.end() || found->second.side != side || found->second.price != price ||
              !is_valid_qty(resting.qty)) {
            return "reference: index disagrees with the queues";
          }
          ++orders;
        }
      }
    }
    if (orders != where_.size()) {
      return "reference: index holds orders that are not queued";
    }
    const std::optional<Price> bid = best_price(Side::buy);
    const std::optional<Price> ask = best_price(Side::sell);
    if (bid && ask && *bid >= *ask) {
      return "reference: book is crossed";
    }
    return std::nullopt;
  }

 private:
  struct Resting {
    OrderId id{};
    OwnerId owner{};
    OrderType type = OrderType::limit;
    Qty qty;
  };
  using Queue = std::list<Resting>;
  using Levels = std::map<Price, Queue>;  // Ascending price on both sides.

  struct Location {
    Side side = Side::buy;
    Price price;
  };

  struct Incoming {
    OrderId id{};
    OwnerId owner{};
    Side side = Side::buy;
    bool any_price = false;
    Price limit;

    /// True if this order may trade at `price`.
    [[nodiscard]] bool accepts(Price price) const noexcept {
      if (any_price) {
        return true;
      }
      return side == Side::buy ? price <= limit : price >= limit;
    }
  };

  [[nodiscard]] Levels& levels(Side side) noexcept { return side == Side::buy ? bids_ : asks_; }
  [[nodiscard]] const Levels& levels(Side side) const noexcept {
    return side == Side::buy ? bids_ : asks_;
  }

  /// Highest bid or lowest ask.
  [[nodiscard]] std::optional<Price> best_price(Side side) const {
    const Levels& book_side = levels(side);
    if (book_side.empty()) {
      return std::nullopt;
    }
    return side == Side::buy ? book_side.rbegin()->first : book_side.begin()->first;
  }

  [[nodiscard]] bool crosses_now(const Incoming& incoming) const {
    const std::optional<Price> best = best_price(opposite(incoming.side));
    return best && incoming.accepts(*best);
  }

  [[nodiscard]] bool is_self_trade(OwnerId taker, OwnerId maker) const noexcept {
    return config_.stp != StpPolicy::none && taker != OwnerId{0} && taker == maker;
  }

  [[nodiscard]] static Queue::iterator find_in(Queue& queue, OrderId id) {
    return std::ranges::find(queue, id, &Resting::id);
  }

  [[nodiscard]] static LevelView view_of(Price price, const Queue& queue) {
    Qty total{0};
    for (const Resting& resting : queue) {
      total += resting.qty;
    }
    return LevelView{price, total, static_cast<std::uint32_t>(queue.size())};
  }

  template <EventSink Sink>
  void reject(OrderId id, Request request, RejectReason reason, Sink& sink) const {
    sink(Rejected{config_.symbol, id, request, reason});
  }

  /// Emits the current state of a level and removes it if it is empty.
  template <EventSink Sink>
  void publish_level(Side side, Price price, Sink& sink) {
    Levels& book_side = levels(side);
    const auto level = book_side.find(price);
    const LevelView view = view_of(price, level->second);
    sink(BookDelta{config_.symbol, side, price, view.qty, view.count});
    if (level->second.empty()) {
      book_side.erase(level);
    }
  }

  template <EventSink Sink>
  void add(Side side, Price price, const Resting& resting, Sink& sink) {
    levels(side)[price].push_back(resting);
    where_[resting.id] = Location{side, price};
    publish_level(side, price, sink);
  }

  /// Quantity an incoming order could trade right now (for fill-or-kill).
  [[nodiscard]] Qty fillable_qty(const Incoming& incoming) const {
    Qty total{0};
    const auto count_level = [&](Price price, const Queue& queue) {
      if (!incoming.accepts(price)) {
        return false;
      }
      for (const Resting& resting : queue) {
        if (is_self_trade(incoming.owner, resting.owner)) {
          if (config_.stp == StpPolicy::cancel_newest) {
            return false;  // Matching would stop at this order.
          }
          continue;  // It would be cancelled, not traded.
        }
        total += resting.qty;
      }
      return true;
    };
    if (incoming.side == Side::buy) {
      for (const auto& [price, queue] : asks_) {
        if (!count_level(price, queue)) {
          break;
        }
      }
    } else {
      for (const auto& [price, queue] : bids_ | std::views::reverse) {
        if (!count_level(price, queue)) {
          break;
        }
      }
    }
    return total;
  }

  /// Trades `qty` of `incoming` against the opposite side; returns what is
  /// left (zero when filled, or when self-trade prevention cancelled it).
  template <EventSink Sink>
  Qty match(const Incoming& incoming, Qty qty, Sink& sink) {
    const Side maker_side = opposite(incoming.side);
    while (qty > Qty{0}) {
      const std::optional<Price> best = best_price(maker_side);
      if (!best || !incoming.accepts(*best)) {
        break;
      }
      Queue& queue = levels(maker_side).at(*best);
      bool touched = false;
      auto it = queue.begin();
      while (it != queue.end() && qty > Qty{0}) {
        if (is_self_trade(incoming.owner, it->owner)) {
          if (config_.stp == StpPolicy::cancel_newest) {
            if (touched) {
              publish_level(maker_side, *best, sink);
            }
            sink(Cancelled{config_.symbol, incoming.id, qty, CancelReason::self_trade});
            return Qty{0};
          }
          sink(Cancelled{config_.symbol, it->id, it->qty, CancelReason::self_trade});
          where_.erase(it->id);
          it = queue.erase(it);
          touched = true;
          continue;
        }
        const Qty traded = std::min(qty, it->qty);
        qty -= traded;
        it->qty -= traded;
        sink(Fill{config_.symbol, incoming.id, it->id, incoming.side, *best, traded, qty, it->qty});
        touched = true;
        if (it->qty == Qty{0}) {
          where_.erase(it->id);
          it = queue.erase(it);
        } else {
          ++it;
        }
      }
      // A level is only entered when it crosses, so something happened here.
      publish_level(maker_side, *best, sink);
    }
    return qty;
  }

  BookConfig config_;
  Levels bids_;
  Levels asks_;
  std::unordered_map<OrderId, Location> where_;
};

}  // namespace ob
