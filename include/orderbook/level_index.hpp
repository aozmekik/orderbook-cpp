// SPDX-License-Identifier: MIT
/// @file
/// The `LevelIndex` concept: how one side of a book finds its price levels.
///
/// A level index maps a `LevelKey` to the handle of a price level and knows
/// which level is best. `BasicOrderBook` is parameterised on it, so the
/// optimised ladder and the `std::map` baseline share every other line of the
/// matching code and benchmarks compare the level structure alone.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <orderbook/detail/pool.hpp>
#include <orderbook/types.hpp>

namespace ob {

/// Ordering key of a price level within one side: smaller keys are better.
/// Asks use the price itself and bids its negation, so "best" is always the
/// minimum and one implementation serves both sides.
using LevelKey = std::int64_t;

/// The key of `price` on `side`. Precondition: `price` is in
/// `[kMinPrice, kMaxPrice]` (so that negating it cannot overflow).
[[nodiscard]] constexpr LevelKey level_key(Side side, Price price) noexcept {
  return side == Side::buy ? -price.value() : price.value();
}

/// The price of `key` on `side` (inverse of `level_key`).
[[nodiscard]] constexpr Price level_price(Side side, LevelKey key) noexcept {
  return Price{side == Side::buy ? -key : key};
}

/// Requirements on a level index. Besides these members, a level index has a
/// `for_each(visit)` member template that calls `visit(LevelKey, Index)` for
/// every level from best to worst until `visit` returns false.
///
/// `reserve(n)` makes room for `n` levels where the structure can, so that a
/// book holding at most `n` levels per side does not allocate in its index.
template <class L>
concept LevelIndex =
    std::constructible_from<L, std::uint32_t> &&
    requires(L& index, const L& cindex, LevelKey key, detail::Index (*make)(), std::size_t levels) {
      index.reserve(levels);
      { cindex.find(key) } -> std::same_as<detail::Index>;
      { index.find_or_insert(key, make) } -> std::same_as<detail::Index>;
      index.erase(key);
      { cindex.best() } -> std::same_as<detail::Index>;
      { cindex.best_key() } -> std::same_as<LevelKey>;
      { cindex.size() } -> std::same_as<std::size_t>;
      { cindex.empty() } -> std::same_as<bool>;
      { cindex.check_invariants() } -> std::same_as<std::optional<std::string>>;
    };

}  // namespace ob
