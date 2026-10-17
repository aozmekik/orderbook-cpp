// SPDX-License-Identifier: MIT
// Market data from events: rebuilds the level-2 book of every symbol from
// BookDelta events alone, keeps a trade tape, and checks the rebuilt book
// against the engine's own depth snapshot at the end.
//
//   ./build/dev/examples/example_market_data

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <type_traits>
#include <vector>

#include <orderbook/orderbook.hpp>

namespace {

/// A level-2 book maintained from deltas: price -> {qty, orders}.
struct L2Book {
  std::map<std::int64_t, ob::LevelView, std::greater<>> bids;
  std::map<std::int64_t, ob::LevelView> asks;

  void apply(const ob::BookDelta& delta) {
    const ob::LevelView level{delta.price, delta.qty, delta.count};
    if (delta.side == ob::Side::buy) {
      update(bids, delta.price.value(), level);
    } else {
      update(asks, delta.price.value(), level);
    }
  }

 private:
  template <class Map>
  static void update(Map& side, std::int64_t price, const ob::LevelView& level) {
    if (level.qty == ob::Qty{0}) {
      side.erase(price);
    } else {
      side.insert_or_assign(price, level);
    }
  }
};

/// Consumes events the way a market-data publisher would.
class Feed {
 public:
  explicit Feed(std::size_t symbols)
      : books_(symbols), volume_(symbols, 0), notional_(symbols, 0) {}

  template <class E>
  void operator()(const E& event) {
    if constexpr (std::is_same_v<E, ob::BookDelta>) {
      books_[index(event.symbol)].apply(event);
    } else if constexpr (std::is_same_v<E, ob::Fill>) {
      const std::size_t i = index(event.symbol);
      volume_[i] += event.qty.value();
      notional_[i] += event.qty.value() * event.price.value();
      ++trades_;
    }
  }

  [[nodiscard]] const L2Book& book(std::size_t i) const { return books_[i]; }
  [[nodiscard]] std::int64_t volume(std::size_t i) const { return volume_[i]; }
  [[nodiscard]] std::int64_t notional(std::size_t i) const { return notional_[i]; }
  [[nodiscard]] std::uint64_t trades() const { return trades_; }

 private:
  static std::size_t index(ob::SymbolId symbol) {
    return static_cast<std::size_t>(ob::to_underlying(symbol));
  }

  std::vector<L2Book> books_;
  std::vector<std::int64_t> volume_;
  std::vector<std::int64_t> notional_;
  std::uint64_t trades_ = 0;
};

template <class Map>
bool same_levels(const Map& mirror, const std::vector<ob::LevelView>& depth) {
  if (mirror.size() != depth.size()) {
    return false;
  }
  std::size_t i = 0;
  for (const auto& entry : mirror) {
    if (!(entry.second == depth[i++])) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  ob::workload::Config config;
  config.commands = 200'000;
  config.symbols = 3;
  config.seed = 7;
  const ob::workload::Workload workload = ob::workload::generate(config);

  ob::Engine engine;
  for (const auto& name : workload.symbols) {
    engine.add_symbol(name);
  }
  Feed feed(workload.symbols.size());
  for (const ob::Command& command : workload.commands) {
    engine.apply(command, feed);
  }

  bool all_match = true;
  for (std::size_t i = 0; i < engine.symbol_count(); ++i) {
    const ob::OrderBook& book = *engine.find_book(static_cast<ob::SymbolId>(i));
    std::vector<ob::LevelView> bids(book.level_count(ob::Side::buy));
    std::vector<ob::LevelView> asks(book.level_count(ob::Side::sell));
    bids.resize(book.depth(ob::Side::buy, bids));
    asks.resize(book.depth(ob::Side::sell, asks));
    const bool match = same_levels(feed.book(i).bids, bids) && same_levels(feed.book(i).asks, asks);
    all_match = all_match && match;
    const std::int64_t volume = feed.volume(i);
    std::printf("%s: %zu bid / %zu ask levels, volume %lld, VWAP %lld ticks, L2 mirror %s\n",
                workload.symbols[i].c_str(), bids.size(), asks.size(),
                static_cast<long long>(volume),
                static_cast<long long>(volume == 0 ? 0 : feed.notional(i) / volume),
                match ? "matches the book" : "DIFFERS");
  }
  std::printf("%llu trades from %zu synthetic commands\n",
              static_cast<unsigned long long>(feed.trades()), workload.commands.size());
  return all_match ? 0 : 1;
}
