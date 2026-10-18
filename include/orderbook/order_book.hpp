// SPDX-License-Identifier: MIT
/// @file
/// `BasicOrderBook`: the optimised single-symbol book and matching engine.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/detail/order_id_map.hpp>
#include <orderbook/detail/pool.hpp>
#include <orderbook/events.hpp>
#include <orderbook/ladder_levels.hpp>
#include <orderbook/level_index.hpp>
#include <orderbook/map_levels.hpp>
#include <orderbook/types.hpp>

/// When non-zero, every mutating call on a book re-checks all of its
/// invariants and aborts on a violation. Defaults to on unless NDEBUG is
/// defined (like `assert`). The check is O(orders + levels) per call.
#ifndef OB_ENABLE_INVARIANT_CHECKS
#ifdef NDEBUG
#define OB_ENABLE_INVARIANT_CHECKS 0
#else
#define OB_ENABLE_INVARIANT_CHECKS 1
#endif
#endif

namespace ob {

namespace detail {

/// Prints `what` and aborts. Used by the debug-build invariant checks.
[[noreturn]] inline void invariant_failure(std::string_view what) noexcept {
  constexpr std::string_view kPrefix = "orderbook: invariant violated: ";
  static_cast<void>(std::fwrite(kPrefix.data(), 1, kPrefix.size(), stderr));
  static_cast<void>(std::fwrite(what.data(), 1, what.size(), stderr));
  static_cast<void>(std::fputc('\n', stderr));
  std::abort();
}

/// Aborts if `book` violates an invariant (only when OB_ENABLE_INVARIANT_CHECKS).
template <class Book>
void verify_invariants([[maybe_unused]] const Book& book) noexcept {
#if OB_ENABLE_INVARIANT_CHECKS
  if (const std::optional<std::string> violation = book.check_invariants()) {
    invariant_failure(*violation);
  }
#endif
}

}  // namespace detail

/// A limit order book for one symbol with price-time priority matching.
///
/// `Levels` decides how each side finds its price levels (`LadderLevels` or
/// `MapLevels`); everything else is shared:
/// - orders and price levels live in index-addressed pools (`detail::Pool`)
///   sized by `BookConfig::initial_capacity` or `reserve`, so a book that
///   stays within that capacity does not allocate on the hot path;
/// - each price level is an intrusive doubly linked FIFO through the orders,
///   so cancelling from the middle of a queue is O(1);
/// - order ids map to orders through an open-addressing hash map whose hash
///   is seeded per book (`BookConfig::hash_seed`), so clients cannot choose
///   ids that collide.
///
/// All mutating calls are `noexcept`: rejections are events, never
/// exceptions. Allocation failure while a pool grows terminates the process.
/// A book is not thread-safe; distinct books are independent.
template <LevelIndex Levels>
class BasicOrderBook {
 public:
  /// Creates an empty book.
  explicit BasicOrderBook(const BookConfig& config = {})
      : config_(config),
        bids_(config.ladder_ticks),
        asks_(config.ladder_ticks),
        ids_(config.initial_capacity,
             config.hash_seed != 0 ? config.hash_seed : detail::random_hash_seed()) {
    reserve(config.initial_capacity);
  }

  /// Processes a new order: validation, matching, then resting or cancelling
  /// the remainder depending on its type. Emits events into `sink`.
  template <EventSink Sink>
  void submit(const NewOrder& order, Sink& sink) noexcept {
    if (const std::optional<RejectReason> reason = validate(order)) {
      reject(order.id, Request::new_order, *reason, sink);
    } else if (lookup(order.id) != detail::kNil) {
      reject(order.id, Request::new_order, RejectReason::duplicate_order_id, sink);
    } else if (can_rest(order.type) && orders_.live() >= config_.max_orders) {
      reject(order.id, Request::new_order, RejectReason::book_full, sink);
    } else if (order.side == Side::buy) {
      execute<Side::buy>(order, sink);
    } else {
      execute<Side::sell>(order, sink);
    }
    detail::verify_invariants(*this);
  }

  /// Cancels the open quantity of a resting order.
  template <EventSink Sink>
  void cancel(const CancelOrder& request, Sink& sink) noexcept {
    const detail::Index node = lookup(request.id);
    if (node == detail::kNil) {
      reject(request.id, Request::cancel, RejectReason::unknown_order, sink);
    } else {
      const OrderNode& order = orders_[node];
      emit_cancelled(order.id, order.qty, CancelReason::requested, sink);
      unlink_resting(node, sink);
    }
    detail::verify_invariants(*this);
  }

  /// Changes the price and/or open quantity of a resting order. See
  /// `ModifyOrder` for the priority rules.
  template <EventSink Sink>
  void modify(const ModifyOrder& request, Sink& sink) noexcept {
    const detail::Index node = lookup(request.id);
    if (const std::optional<RejectReason> reason = validate(request)) {
      reject(request.id, Request::modify, *reason, sink);
    } else if (node == detail::kNil) {
      reject(request.id, Request::modify, RejectReason::unknown_order, sink);
    } else if (request.price == levels_[orders_[node].level].price) {
      resize(node, request.qty, sink);
    } else if (orders_[node].side == Side::buy) {
      reprice<Side::buy>(node, request, sink);
    } else {
      reprice<Side::sell>(node, request, sink);
    }
    detail::verify_invariants(*this);
  }

  /// The best level of `side`, if any.
  [[nodiscard]] std::optional<LevelView> best(Side side) const noexcept {
    const detail::Index level = index_of(side).best();
    if (level == detail::kNil) {
      return std::nullopt;
    }
    return view_of(levels_[level]);
  }

  /// The best bid level, if any.
  [[nodiscard]] std::optional<LevelView> best_bid() const noexcept { return best(Side::buy); }

  /// The best ask level, if any.
  [[nodiscard]] std::optional<LevelView> best_ask() const noexcept { return best(Side::sell); }

  /// Writes up to `out.size()` levels of `side`, best first, into `out`;
  /// returns how many were written. Does not allocate.
  [[nodiscard]] std::size_t depth(Side side, std::span<LevelView> out) const noexcept {
    std::size_t written = 0;
    if (out.empty()) {
      return written;
    }
    index_of(side).for_each([&](LevelKey /*key*/, detail::Index level) {
      out[written++] = view_of(levels_[level]);
      return written < out.size();
    });
    return written;
  }

  /// The resting order with id `id`, if any.
  [[nodiscard]] std::optional<OrderView> find(OrderId id) const noexcept {
    const detail::Index node = lookup(id);
    if (node == detail::kNil) {
      return std::nullopt;
    }
    return view_of(node);
  }

  /// Calls `visit(OrderView)` for every order resting at `price` on `side`,
  /// in time priority (first in the queue first). Visits nothing for a price
  /// outside `[kMinPrice, kMaxPrice]`, where no order can rest.
  template <class Visit>
  void for_each_order(Side side, Price price, Visit visit) const {
    if (!is_valid_price(price)) {
      return;
    }
    const detail::Index level = index_of(side).find(level_key(side, price));
    if (level == detail::kNil) {
      return;
    }
    for (detail::Index node = levels_[level].head; node != detail::kNil;
         node = orders_[node].next) {
      visit(view_of(node));
    }
  }

  /// Number of resting orders.
  [[nodiscard]] std::size_t order_count() const noexcept { return orders_.live(); }

  /// Number of price levels on `side`.
  [[nodiscard]] std::size_t level_count(Side side) const noexcept { return index_of(side).size(); }

  /// The configuration the book was created with.
  [[nodiscard]] const BookConfig& config() const noexcept { return config_; }

  /// Reserves memory for `orders` resting orders, and for as many price
  /// levels on each side, so that the book does not allocate until it holds
  /// more than `orders` orders (`MapOrderBook` still allocates a tree node
  /// per new price level).
  void reserve(std::size_t orders) {
    orders_.reserve(orders);
    levels_.reserve(orders);
    ids_.reserve(orders);
    bids_.reserve(orders);
    asks_.reserve(orders);
  }

  /// Checks every invariant of the book; returns a description of the first
  /// violation, or `std::nullopt`. O(orders + levels). Invariants:
  /// - the book is not crossed (best bid < best ask);
  /// - every level is non-empty, indexed under its own price, and its total
  ///   and count equal the sum over its queue;
  /// - queues are well-formed doubly linked lists of valid resting orders;
  /// - the id map, the pools and the level indexes agree with the queues;
  /// - quantity is conserved: accepted quantity (plus increases) equals
  ///   filled quantity (counted for both sides) plus cancelled and reduced
  ///   quantity plus the quantity still resting.
  [[nodiscard]] std::optional<std::string> check_invariants() const {
    std::size_t orders_seen = 0;
    std::uint64_t resting = 0;
    for (const Side side : {Side::buy, Side::sell}) {
      if (std::optional<std::string> violation = check_side(side, orders_seen, resting)) {
        return violation;
      }
    }
    if (orders_seen != orders_.live()) {
      return "order pool live count disagrees with the queued orders";
    }
    if (ids_.size() != orders_.live()) {
      return "id map size disagrees with the number of resting orders";
    }
    if (levels_.live() != bids_.size() + asks_.size()) {
      return "level pool live count disagrees with the level indexes";
    }
    const std::optional<LevelView> bid = best_bid();
    const std::optional<LevelView> ask = best_ask();
    if (bid && ask && bid->price >= ask->price) {
      return "book is crossed";
    }
    if (qty_in_ - qty_out_ != resting) {
      return "quantity is not conserved";
    }
    return std::nullopt;
  }

 private:
  struct OrderNode {
    OrderId id{};
    Qty qty;
    OwnerId owner{};
    detail::Index prev = detail::kNil;
    detail::Index next = detail::kNil;
    detail::Index level = detail::kNil;
    Side side = Side::buy;
    OrderType type = OrderType::limit;
  };

  struct Level {
    Price price;
    Qty total;
    detail::Index head = detail::kNil;
    detail::Index tail = detail::kNil;
    std::uint32_t count = 0;
  };

  // Cache footprint: 40 and 32 bytes on 64-bit platforms.
  static_assert(sizeof(OrderNode) <= 40);
  static_assert(sizeof(Level) <= 32);

  /// The incoming side of a match. `limit` is the worst acceptable maker
  /// key: maker levels with a larger key do not cross.
  struct Taker {
    OrderId id;
    OwnerId owner;
    LevelKey limit;
  };

  /// Limit key of a market order: crosses every level.
  static constexpr LevelKey kAnyPrice = std::numeric_limits<LevelKey>::max();

  template <Side S>
  [[nodiscard]] Levels& index_of() noexcept {
    if constexpr (S == Side::buy) {
      return bids_;
    } else {
      return asks_;
    }
  }

  template <Side S>
  [[nodiscard]] const Levels& index_of() const noexcept {
    if constexpr (S == Side::buy) {
      return bids_;
    } else {
      return asks_;
    }
  }

  [[nodiscard]] Levels& index_of(Side side) noexcept { return side == Side::buy ? bids_ : asks_; }

  [[nodiscard]] const Levels& index_of(Side side) const noexcept {
    return side == Side::buy ? bids_ : asks_;
  }

  [[nodiscard]] detail::Index lookup(OrderId id) const noexcept {
    return id == OrderId{0} ? detail::kNil : ids_.find(to_underlying(id));
  }

  [[nodiscard]] static LevelView view_of(const Level& level) noexcept {
    return LevelView{level.price, level.total, level.count};
  }

  [[nodiscard]] OrderView view_of(detail::Index node) const noexcept {
    const OrderNode& order = orders_[node];
    return OrderView{order.id, order.owner, order.side, order.type, levels_[order.level].price,
                     order.qty};
  }

  [[nodiscard]] bool stp_applies(OwnerId owner) const noexcept {
    return config_.stp != StpPolicy::none && owner != OwnerId{0};
  }

  /// True if a taker on side `S` with limit key `limit` would match now.
  template <Side S>
  [[nodiscard]] bool crosses(LevelKey limit) const noexcept {
    const Levels& makers = index_of<opposite(S)>();
    return !makers.empty() && makers.best_key() <= limit;
  }

  // --- Event emission (keeps the conservation counters in step) ------------

  template <EventSink Sink>
  void reject(OrderId id, Request request, RejectReason reason, Sink& sink) noexcept {
    sink(Rejected{config_.symbol, id, request, reason});
  }

  template <EventSink Sink>
  void emit_cancelled(OrderId id, Qty qty, CancelReason reason, Sink& sink) noexcept {
    qty_out_ += static_cast<std::uint64_t>(qty.value());
    sink(Cancelled{config_.symbol, id, qty, reason});
  }

  template <EventSink Sink>
  void emit_delta(Side side, const Level& level, Sink& sink) noexcept {
    sink(BookDelta{config_.symbol, side, level.price, level.total, level.count});
  }

  // --- Queue primitives -----------------------------------------------------

  void link_back(Level& level, detail::Index node) noexcept {
    OrderNode& order = orders_[node];
    order.prev = level.tail;
    order.next = detail::kNil;
    if (level.tail != detail::kNil) {
      orders_[level.tail].next = node;
    } else {
      level.head = node;
    }
    level.tail = node;
    level.total += order.qty;
    ++level.count;
  }

  void unlink(Level& level, detail::Index node) noexcept {
    const OrderNode& order = orders_[node];
    if (order.prev != detail::kNil) {
      orders_[order.prev].next = order.next;
    } else {
      level.head = order.next;
    }
    if (order.next != detail::kNil) {
      orders_[order.next].prev = order.prev;
    } else {
      level.tail = order.prev;
    }
    level.total -= order.qty;
    --level.count;
  }

  /// Unlinks `node` from `level` and frees it (and its id).
  void erase_order(Level& level, detail::Index node) noexcept {
    unlink(level, node);
    [[maybe_unused]] const bool erased = ids_.erase(to_underlying(orders_[node].id));
    assert(erased);
    orders_.release(node);
  }

  /// Removes the price level `level` of `side` from its index and frees it.
  void erase_level(Side side, detail::Index level) noexcept {
    index_of(side).erase(level_key(side, levels_[level].price));
    levels_.release(level);
  }

  /// Removes resting order `node` (its events already emitted), then emits
  /// the level update and drops the level if it became empty.
  template <EventSink Sink>
  void unlink_resting(detail::Index node, Sink& sink) noexcept {
    const Side side = orders_[node].side;
    const detail::Index level_index = orders_[node].level;
    Level& level = levels_[level_index];
    erase_order(level, node);
    emit_delta(side, level, sink);
    if (level.count == 0) {
      erase_level(side, level_index);
    }
  }

  // --- New orders -----------------------------------------------------------

  template <Side S, EventSink Sink>
  void execute(const NewOrder& order, Sink& sink) noexcept {
    const bool is_market = order.type == OrderType::market;
    const Taker taker{order.id, order.owner,
                      is_market ? kAnyPrice : level_key(opposite(S), order.price)};
    if (order.type == OrderType::post_only && crosses<S>(taker.limit)) {
      reject(order.id, Request::new_order, RejectReason::post_only_would_cross, sink);
      return;
    }
    qty_in_ += static_cast<std::uint64_t>(order.qty.value());
    sink(Accepted{config_.symbol, order.id, order.owner, S, order.type,
                  is_market ? Price{0} : order.price, order.qty});
    if (order.type == OrderType::fok && !can_fill<S>(taker, order.qty)) {
      emit_cancelled(order.id, order.qty, CancelReason::fok_unfillable, sink);
      return;
    }
    const Qty remaining = match<S>(taker, order.qty, sink);
    if (remaining == Qty{0}) {
      return;
    }
    if (can_rest(order.type)) {
      rest<S>(order.id, order.owner, order.type, order.price, remaining, sink);
    } else {
      assert(order.type != OrderType::fok);
      emit_cancelled(order.id, remaining, CancelReason::ioc_remainder, sink);
    }
  }

  /// True if a taker on side `S` could fill `qty` right now, taking
  /// self-trade prevention into account. Never changes the book, which is
  /// what makes fill-or-kill atomic.
  template <Side S>
  [[nodiscard]] bool can_fill(const Taker& taker, Qty qty) const noexcept {
    const bool stp = stp_applies(taker.owner);
    Qty available{0};
    index_of<opposite(S)>().for_each([&](LevelKey key, detail::Index level_index) {
      if (key > taker.limit) {
        return false;
      }
      const Level& level = levels_[level_index];
      if (!stp) {
        available += level.total;
        return available < qty;
      }
      for (detail::Index node = level.head; node != detail::kNil; node = orders_[node].next) {
        const OrderNode& maker = orders_[node];
        if (maker.owner == taker.owner) {
          if (config_.stp == StpPolicy::cancel_newest) {
            return false;  // Matching would stop here.
          }
          continue;  // cancel_oldest: the maker would be cancelled, not traded.
        }
        available += maker.qty;
        if (available >= qty) {
          return false;
        }
      }
      return true;
    });
    return available >= qty;
  }

  /// Matches a taker of side `S` against the opposite side. Returns the open
  /// quantity left: zero if it filled completely or self-trade prevention
  /// cancelled it (in which case the `Cancelled` event was emitted here).
  template <Side S, EventSink Sink>
  Qty match(const Taker& taker, Qty qty, Sink& sink) noexcept {
    constexpr Side kMakerSide = opposite(S);
    Levels& makers = index_of<kMakerSide>();
    const bool stp = stp_applies(taker.owner);
    while (qty > Qty{0}) {
      const detail::Index level_index = makers.best();
      if (level_index == detail::kNil || makers.best_key() > taker.limit) {
        break;
      }
      Level& level = levels_[level_index];
      bool touched = false;
      bool self_trade_stop = false;
      for (detail::Index node = level.head; node != detail::kNil && qty > Qty{0};) {
        OrderNode& maker = orders_[node];
        const detail::Index next = maker.next;
        if (stp && maker.owner == taker.owner) {
          if (config_.stp == StpPolicy::cancel_newest) {
            self_trade_stop = true;
            break;
          }
          emit_cancelled(maker.id, maker.qty, CancelReason::self_trade, sink);
          erase_order(level, node);
        } else {
          const Qty traded = std::min(qty, maker.qty);
          qty -= traded;
          maker.qty -= traded;
          level.total -= traded;
          qty_out_ += 2U * static_cast<std::uint64_t>(traded.value());
          sink(Fill{config_.symbol, taker.id, maker.id, S, level.price, traded, qty, maker.qty});
          if (maker.qty == Qty{0}) {
            erase_order(level, node);
          }
        }
        touched = true;
        node = next;
      }
      if (touched) {
        emit_delta(kMakerSide, level, sink);
      }
      if (level.count == 0) {
        erase_level(kMakerSide, level_index);
      }
      if (self_trade_stop) {
        emit_cancelled(taker.id, qty, CancelReason::self_trade, sink);
        return Qty{0};
      }
    }
    return qty;
  }

  /// Adds an order of side `S` to the back of the queue at `price`.
  template <Side S, EventSink Sink>
  void rest(OrderId id, OwnerId owner, OrderType type, Price price, Qty qty, Sink& sink) {
    const detail::Index level_index = index_of<S>().find_or_insert(level_key(S, price), [&] {
      const detail::Index fresh = levels_.allocate();
      levels_[fresh] = Level{price, Qty{0}, detail::kNil, detail::kNil, 0};
      return fresh;
    });
    const detail::Index node = orders_.allocate();
    orders_[node] = OrderNode{id, qty, owner, detail::kNil, detail::kNil, level_index, S, type};
    Level& level = levels_[level_index];
    link_back(level, node);
    ids_.insert(to_underlying(id), node);
    emit_delta(S, level, sink);
  }

  // --- Modify ---------------------------------------------------------------

  /// Same price: a decrease keeps priority, an increase goes to the back.
  template <EventSink Sink>
  void resize(detail::Index node, Qty qty, Sink& sink) noexcept {
    OrderNode& order = orders_[node];
    Level& level = levels_[order.level];
    if (qty <= order.qty) {
      const Qty reduced = order.qty - qty;
      order.qty = qty;
      level.total -= reduced;
      qty_out_ += static_cast<std::uint64_t>(reduced.value());
      sink(Modified{config_.symbol, order.id, level.price, qty, true});
      if (reduced > Qty{0}) {
        emit_delta(order.side, level, sink);
      }
      return;
    }
    qty_in_ += static_cast<std::uint64_t>((qty - order.qty).value());
    unlink(level, node);
    order.qty = qty;
    link_back(level, node);
    sink(Modified{config_.symbol, order.id, level.price, qty, false});
    emit_delta(order.side, level, sink);
  }

  /// New price: leave the old level, then match and rest like a new order.
  template <Side S, EventSink Sink>
  void reprice(detail::Index node, const ModifyOrder& request, Sink& sink) noexcept {
    const OrderNode order = orders_[node];  // Copy: the node is freed below.
    const Taker taker{order.id, order.owner, level_key(opposite(S), request.price)};
    if (order.type == OrderType::post_only && crosses<S>(taker.limit)) {
      reject(order.id, Request::modify, RejectReason::post_only_would_cross, sink);
      return;
    }
    sink(Modified{config_.symbol, order.id, request.price, request.qty, false});
    qty_out_ += static_cast<std::uint64_t>(order.qty.value());
    qty_in_ += static_cast<std::uint64_t>(request.qty.value());
    unlink_resting(node, sink);
    const Qty remaining = match<S>(taker, request.qty, sink);
    if (remaining > Qty{0}) {
      rest<S>(order.id, order.owner, order.type, request.price, remaining, sink);
    }
  }

  // --- Invariants -----------------------------------------------------------

  [[nodiscard]] std::optional<std::string> check_side(Side side, std::size_t& orders_seen,
                                                      std::uint64_t& resting) const {
    const Levels& index = index_of(side);
    if (std::optional<std::string> violation = index.check_invariants()) {
      return violation;
    }
    std::optional<std::string> failure;
    std::size_t levels_seen = 0;
    LevelKey previous = std::numeric_limits<LevelKey>::min();
    index.for_each([&](LevelKey key, detail::Index level_index) {
      const Level& level = levels_[level_index];
      if (levels_seen++ > 0 && key <= previous) {
        failure = "levels are not visited in priority order";
      } else if (level_key(side, level.price) != key) {
        failure = "level indexed under a key that is not its price";
      } else if (level.count == 0 || level.head == detail::kNil) {
        failure = "empty level is still indexed";
      } else {
        failure = check_queue(side, level_index, orders_seen, resting);
      }
      previous = key;
      return !failure;
    });
    if (!failure && levels_seen != index.size()) {
      failure = "level index size disagrees with its iteration";
    }
    return failure;
  }

  [[nodiscard]] std::optional<std::string> check_queue(Side side, detail::Index level_index,
                                                       std::size_t& orders_seen,
                                                       std::uint64_t& resting) const {
    const Level& level = levels_[level_index];
    Qty total{0};
    std::uint32_t count = 0;
    detail::Index previous = detail::kNil;
    for (detail::Index node = level.head; node != detail::kNil; node = orders_[node].next) {
      const OrderNode& order = orders_[node];
      if (count++ > orders_.live()) {
        return "cycle in a level queue";
      }
      if (order.prev != previous) {
        return "broken back link in a level queue";
      }
      if (order.level != level_index || order.side != side) {
        return "order does not point back to its level";
      }
      if (!is_valid_qty(order.qty) || !can_rest(order.type)) {
        return "resting order has an invalid quantity or type";
      }
      if (lookup(order.id) != node) {
        return "id map does not point to the resting order";
      }
      total += order.qty;
      previous = node;
    }
    if (level.tail != previous) {
      return "level tail is not the last order of its queue";
    }
    if (total != level.total || count != level.count) {
      return "level total or count is not the sum over its queue";
    }
    orders_seen += count;
    resting += static_cast<std::uint64_t>(total.value());
    return std::nullopt;
  }

  BookConfig config_;
  Levels bids_;
  Levels asks_;
  detail::Pool<OrderNode> orders_;
  detail::Pool<Level> levels_;
  detail::OrderIdMap ids_;
  // Conservation counters, modulo 2^64 so that they may wrap in long runs.
  std::uint64_t qty_in_ = 0;   // Accepted quantity plus modify increases.
  std::uint64_t qty_out_ = 0;  // 2 x filled + cancelled + modify decreases.
};

/// The optimised book: tick-indexed ladder around the touch.
using OrderBook = BasicOrderBook<LadderLevels>;

/// The same book with `std::map` price levels; the benchmark baseline.
using MapOrderBook = BasicOrderBook<MapLevels>;

}  // namespace ob
