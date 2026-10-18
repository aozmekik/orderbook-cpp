// SPDX-License-Identifier: MIT
/// @file
/// `MapLevels`: the `std::map` baseline level index.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <orderbook/detail/pool.hpp>
#include <orderbook/level_index.hpp>

namespace ob {

/// Level index backed by a red-black tree (`std::map`): O(log n) everywhere,
/// one heap allocation per new price level. It is the textbook structure and
/// the baseline the benchmarks compare `LadderLevels` against.
class MapLevels {
 public:
  /// Creates an empty index. The argument (a ladder window) is ignored; it
  /// exists so that every level index is constructible the same way.
  explicit MapLevels(std::uint32_t /*window_ticks*/ = 0) {}

  /// The level stored for `key`, or `detail::kNil`.
  [[nodiscard]] detail::Index find(LevelKey key) const noexcept {
    const auto it = levels_.find(key);
    return it == levels_.end() ? detail::kNil : it->second;
  }

  /// The level stored for `key`; if there is none, stores and returns `make()`.
  template <class Make>
  detail::Index find_or_insert(LevelKey key, Make make) {
    const auto [it, inserted] = levels_.try_emplace(key, detail::kNil);
    if (inserted) {
      it->second = make();
    }
    return it->second;
  }

  /// Removes `key`. Precondition: `key` is present.
  void erase(LevelKey key) noexcept {
    [[maybe_unused]] const std::size_t erased = levels_.erase(key);
    assert(erased == 1);
  }

  /// The best level, or `detail::kNil` if the index is empty.
  [[nodiscard]] detail::Index best() const noexcept {
    return levels_.empty() ? detail::kNil : levels_.begin()->second;
  }

  /// The key of the best level. Precondition: `!empty()`.
  [[nodiscard]] LevelKey best_key() const noexcept {
    assert(!levels_.empty());
    return levels_.begin()->first;
  }

  /// Calls `visit(key, level)` from the best level to the worst until it
  /// returns false.
  template <class Visit>
  void for_each(Visit visit) const {
    for (const auto& [key, level] : levels_) {
      if (!visit(key, level)) {
        return;
      }
    }
  }

  /// Number of levels.
  [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

  /// True if there are no levels.
  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

  /// A `std::map` is consistent by construction; always `std::nullopt`.
  [[nodiscard]] static std::optional<std::string> check_invariants() { return std::nullopt; }

 private:
  std::map<LevelKey, detail::Index> levels_;
};

}  // namespace ob
