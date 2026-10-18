// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/random.hpp>
#include <orderbook/types.hpp>
#include <orderbook/workload.hpp>

namespace ob::workload {
namespace {

constexpr std::int64_t kLot = 100;

/// Live order ids of one symbol with O(1) insert, erase and uniform pick.
class LiveSet {
 public:
  void add(OrderId id) {
    positions_.emplace(id, ids_.size());
    ids_.push_back(id);
  }

  bool remove(OrderId id) {
    const auto it = positions_.find(id);
    if (it == positions_.end()) {
      return false;
    }
    const std::size_t position = it->second;
    positions_.erase(it);
    const OrderId last = ids_.back();
    ids_.pop_back();
    if (last != id) {
      ids_[position] = last;
      positions_[last] = position;
    }
    return true;
  }

  [[nodiscard]] OrderId pick(Rng& rng) const { return ids_[rng.below(ids_.size())]; }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

 private:
  std::vector<OrderId> ids_;
  std::unordered_map<OrderId, std::size_t> positions_;
};

/// Samples a distance d in [0, n) with probability proportional to 1/(d+1),
/// using integer weights so that every platform draws the same values.
class DistanceTable {
 public:
  explicit DistanceTable(std::uint32_t max_distance) {
    const std::uint32_t n = std::max<std::uint32_t>(1, max_distance);
    cumulative_.reserve(n);
    std::uint64_t total = 0;
    for (std::uint64_t d = 0; d < n; ++d) {
      total += kScale / (d + 1);
      cumulative_.push_back(total);
    }
  }

  [[nodiscard]] std::int64_t sample(Rng& rng) const {
    const std::uint64_t r = rng.below(cumulative_.back());
    return std::ranges::upper_bound(cumulative_, r) - cumulative_.begin();
  }

 private:
  static constexpr std::uint64_t kScale = std::uint64_t{1} << 20U;
  std::vector<std::uint64_t> cumulative_;
};

/// Event sink that keeps the live sets and statistics up to date.
class Tracker {
 public:
  Tracker(std::vector<LiveSet>& live, Stats& stats) noexcept : live_(&live), stats_(&stats) {}

  void operator()(const Fill& fill) {
    ++stats_->fills;
    if (fill.maker_remaining == Qty{0} && set_of(fill.symbol).remove(fill.maker_id)) {
      ++stats_->ended_filled;
    }
  }

  void operator()(const Cancelled& cancelled) {
    if (set_of(cancelled.symbol).remove(cancelled.id)) {
      ++stats_->ended_cancelled;
    }
  }

  template <class E>
  void operator()(const E& /*event*/) const noexcept {}

 private:
  LiveSet& set_of(SymbolId symbol) {
    return (*live_)[static_cast<std::size_t>(to_underlying(symbol))];
  }

  std::vector<LiveSet>* live_;
  Stats* stats_;
};

/// Generates the commands of one run.
class Generator {
 public:
  explicit Generator(const Config& config)
      : config_(config),
        rng_(config.seed),
        symbol_count_(std::max<std::uint32_t>(1, config.symbols)),
        target_orders_(std::max<std::uint32_t>(1, config.target_orders)),
        engine_(book_config(config)),
        live_(symbol_count_),
        reference_(symbol_count_, config.start_price),
        distance_(config.max_distance),
        tracker_(live_, workload_.stats) {
    for (std::uint32_t i = 1; i <= symbol_count_; ++i) {
      workload_.symbols.push_back("SYM" + std::to_string(i));
      static_cast<void>(engine_.add_symbol(workload_.symbols.back()));
    }
  }

  Workload run() && {
    workload_.commands.reserve(config_.commands);
    for (std::uint64_t n = 0; n < config_.commands; ++n) {
      step();
    }
    for (const LiveSet& set : live_) {
      workload_.stats.live_at_end += set.size();
    }
    return std::move(workload_);
  }

 private:
  static BookConfig book_config(const Config& config) {
    BookConfig book;
    book.initial_capacity = std::max<std::uint32_t>(1024, config.target_orders * 2);
    return book;
  }

  void step() {
    const auto index = static_cast<std::size_t>(rng_.below(symbol_count_));
    const auto symbol = static_cast<SymbolId>(index);
    const ReferenceBook& book = *engine_.find_book(symbol);
    LiveSet& live = live_[index];
    const std::optional<LevelView> bid = book.best_bid();
    const std::optional<LevelView> ask = book.best_ask();
    if (bid && ask) {
      reference_[index] = (bid->price.value() + ask->price.value()) / 2;
    }

    const std::uint64_t live_count = live.size();
    const std::uint64_t passive = config_.mix.passive;
    const std::uint64_t aggressive = config_.mix.aggressive;
    // Every live order is equally likely to be cancelled, so the cancel
    // weight grows with the book; it equals mix.cancel at target_orders.
    const std::uint64_t cancel = config_.mix.cancel * live_count / target_orders_;
    const std::uint64_t modify = live_count == 0 ? 0 : config_.mix.modify;
    const std::uint64_t total = passive + aggressive + cancel + modify;

    const std::uint64_t pick = total == 0 ? 0 : rng_.below(total);
    Command command;
    if (pick < passive || total == 0) {
      command = passive_order(symbol, bid, ask, reference_[index]);
    } else if (pick < passive + aggressive) {
      command = aggressive_order(symbol, bid, ask, reference_[index]);
    } else if (pick < passive + aggressive + cancel) {
      command = CancelOrder{symbol, live.pick(rng_)};
    } else {
      const OrderId id = live.pick(rng_);
      const std::optional<OrderView> order = book.find(id);  // Always live.
      command = order ? Command{modify_order(symbol, *order)} : Command{CancelOrder{symbol, id}};
    }

    engine_.apply(command, tracker_);
    if (const auto* order = std::get_if<NewOrder>(&command)) {
      ++workload_.stats.new_orders;
      if (book.find(order->id)) {
        live.add(order->id);
        ++workload_.stats.rested;
      }
    } else if (std::holds_alternative<CancelOrder>(command)) {
      ++workload_.stats.cancels;
    } else {
      ++workload_.stats.modifies;
      // A repriced order that crossed may have been filled as a taker.
      const OrderId id = std::get<ModifyOrder>(command).id;
      if (!book.find(id) && live.remove(id)) {
        ++workload_.stats.ended_filled;
      }
    }
    workload_.commands.push_back(command);
  }

  OwnerId owner() {
    const std::uint64_t owners = std::max<std::uint32_t>(1, config_.owners);
    return OwnerId{static_cast<std::uint32_t>(1 + rng_.below(owners))};
  }

  Qty lots(std::uint64_t max_lots) {
    return Qty{kLot * (1 + static_cast<std::int64_t>(rng_.below(max_lots)))};
  }

  NewOrder passive_order(SymbolId symbol, const std::optional<LevelView>& bid,
                         const std::optional<LevelView>& ask, std::int64_t reference) {
    NewOrder order;
    order.symbol = symbol;
    order.id = OrderId{next_id_++};
    order.owner = owner();
    order.side = rng_.below(2) == 0 ? Side::buy : Side::sell;
    order.type =
        rng_.chance(config_.post_only_percent, 100) ? OrderType::post_only : OrderType::limit;
    const std::int64_t distance = distance_.sample(rng_);
    const std::int64_t anchor = passive_anchor(order.side, bid, ask, reference);
    order.price = Price{order.side == Side::buy ? anchor - distance : anchor + distance};
    order.qty = lots(10);
    return order;
  }

  NewOrder aggressive_order(SymbolId symbol, const std::optional<LevelView>& bid,
                            const std::optional<LevelView>& ask, std::int64_t reference) {
    ++workload_.stats.aggressive;
    NewOrder order;
    order.symbol = symbol;
    order.id = OrderId{next_id_++};
    order.owner = owner();
    order.side = rng_.below(2) == 0 ? Side::buy : Side::sell;
    order.qty = lots(5);
    const std::uint64_t kind = rng_.below(100);
    if (kind < config_.market_percent) {
      order.type = OrderType::market;
      return order;
    }
    order.type = kind < std::uint64_t{config_.market_percent} + config_.fok_percent
                     ? OrderType::fok
                     : OrderType::ioc;
    const auto through = static_cast<std::int64_t>(rng_.below(3));
    order.price = Price{marketable_price(order.side, bid, ask, reference, through)};
    return order;
  }

  /// Where a passive order of `side` starts before stepping back from the
  /// touch: one tick inside the opposite best, else at its own best, else
  /// one tick from the reference price.
  static std::int64_t passive_anchor(Side side, const std::optional<LevelView>& bid,
                                     const std::optional<LevelView>& ask,
                                     std::int64_t reference) noexcept {
    if (side == Side::buy) {
      if (ask) {
        return ask->price.value() - 1;
      }
      return bid ? bid->price.value() : reference - 1;
    }
    if (bid) {
      return bid->price.value() + 1;
    }
    return ask ? ask->price.value() : reference + 1;
  }

  /// Limit price of a marketable IOC/FOK: `through` ticks past the opposite
  /// best; without one, it cannot cross.
  static std::int64_t marketable_price(Side side, const std::optional<LevelView>& bid,
                                       const std::optional<LevelView>& ask, std::int64_t reference,
                                       std::int64_t through) noexcept {
    if (side == Side::buy) {
      if (ask) {
        return ask->price.value() + through;
      }
      return bid ? bid->price.value() : reference;
    }
    if (bid) {
      return bid->price.value() - through;
    }
    return ask ? ask->price.value() : reference;
  }

  ModifyOrder modify_order(SymbolId symbol, const OrderView& order) {
    ModifyOrder request{symbol, order.id, order.price, order.qty};
    const std::uint64_t kind = rng_.below(10);
    if (kind < 6 && order.qty > Qty{1}) {
      request.qty = Qty{1 + static_cast<std::int64_t>(
                                rng_.below(static_cast<std::uint64_t>(order.qty.value() - 1)))};
    } else if (kind < 8) {
      request.qty = std::min(order.qty + lots(3), kMaxOrderQty);
    } else {
      const auto delta = 1 + static_cast<std::int64_t>(rng_.below(3));
      const bool away = rng_.below(2) == 0;
      const bool down = (order.side == Side::buy) == away;
      request.price = Price{order.price.value() + (down ? -delta : delta)};
    }
    return request;
  }

  Config config_;
  Rng rng_;
  std::uint32_t symbol_count_;
  std::uint32_t target_orders_;
  // The reference book: simple, and free of the debug-build invariant checks
  // that would make generating large workloads slow.
  ReferenceEngine engine_;
  std::vector<LiveSet> live_;
  std::vector<std::int64_t> reference_;
  DistanceTable distance_;
  Workload workload_;
  Tracker tracker_;
  std::uint64_t next_id_ = 1;
};

/// Ticks beyond `max_distance` that the generator may step from a price:
/// one inside the spread, two through the touch, three for a reprice.
constexpr std::int64_t kPriceMargin = 8;

}  // namespace

std::int64_t max_start_price(std::uint32_t max_distance) noexcept {
  return kMaxPrice.value() - std::int64_t{max_distance} - kPriceMargin;
}

std::optional<std::string> config_error(const Config& config) {
  if (config.max_distance == 0 || config.max_distance > kMaxDistance) {
    return "the maximum distance must be between 1 and " + std::to_string(kMaxDistance) + " ticks";
  }
  const std::int64_t limit = max_start_price(config.max_distance);
  if (config.start_price < -limit || config.start_price > limit) {
    return "the start price must be between " + std::to_string(-limit) + " and " +
           std::to_string(limit) + " ticks for a maximum distance of " +
           std::to_string(config.max_distance);
  }
  if (config.post_only_percent > 100) {
    return "the post-only share must be at most 100%";
  }
  if (std::uint64_t{config.market_percent} + config.fok_percent > 100) {
    return "the market and fill-or-kill shares must add up to at most 100%";
  }
  return std::nullopt;
}

Workload generate(const Config& config) {
  if (const std::optional<std::string> error = config_error(config)) {
    throw std::invalid_argument("workload: " + *error);
  }
  return Generator(config).run();
}

}  // namespace ob::workload
