// SPDX-License-Identifier: MIT
/// @file
/// `LadderLevels`: a tick-indexed ring around the best price with a sorted
/// overflow for everything further away. See docs/adr/0001-price-level-ladder.md.

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <orderbook/detail/pool.hpp>
#include <orderbook/level_index.hpp>

namespace ob {

/// Level index optimised for activity near the touch.
///
/// Keys in the window `[base, base + W)` live in a ring of `W` slots indexed
/// by `key mod W`, with one bit per slot in an occupancy bitmap; finding the
/// next level after the best one is a scan over 64-bit words. Keys at or past
/// `base + W` live in a vector sorted from worst to best, so the level nearest
/// the window is at the back. No key is ever below `base`.
///
/// The window follows the touch:
/// - a key better than `base` shifts the window down so that the key sits
///   `W/8` ticks above `base`, evicting the far end into the overflow;
/// - when the best key drifts more than `W/2` ticks above `base`, the window
///   shifts up to leave `W/8` ticks of headroom again, pulling overflow levels
///   that now fit into the ring;
/// - the ring is never empty while the overflow has levels: emptying the ring
///   re-centres the window on the best overflow level.
///
/// Every operation near the touch is O(1); the bitmap scan after the best
/// level empties is O(gap / 64). Work in the overflow is O(log n) to find and
/// O(n) to insert or erase, which is the price of keeping it compact and
/// allocation-free once its capacity has been reached.
class LadderLevels {
 public:
  /// Smallest window, in ticks.
  static constexpr std::uint32_t kMinWindow = 64;
  /// Largest window, in ticks (4 MiB of slots per side).
  static constexpr std::uint32_t kMaxWindow = std::uint32_t{1} << 20;

  /// Creates an empty index whose window spans `window_ticks` (rounded up to
  /// a power of two and clamped to `[kMinWindow, kMaxWindow]`).
  explicit LadderLevels(std::uint32_t window_ticks = 4096)
      : window_(std::bit_ceil(std::clamp(window_ticks, kMinWindow, kMaxWindow))),
        headroom_(window_ / 8),
        recenter_distance_(window_ / 2),
        slots_(window_, detail::kNil),
        bits_(window_ / 64, 0) {
    overflow_.reserve(64);
  }

  /// The level stored for `key`, or `detail::kNil`.
  [[nodiscard]] detail::Index find(LevelKey key) const noexcept {
    if (in_window(key)) {
      return slots_[slot_of(key)];
    }
    if (key < base_) {
      return detail::kNil;
    }
    return overflow_find(key);
  }

  /// The level stored for `key`; if there is none, stores and returns
  /// `make()`. Allocates only if the overflow outgrows its capacity.
  template <class Make>
  detail::Index find_or_insert(LevelKey key, Make&& make) {
    if (ring_count_ == 0) {
      assert(overflow_.empty());
      base_ = key - headroom_;
      return place(key, make());
    }
    if (key < base_) {
      shift_down(key - headroom_);
      return place(key, make());
    }
    if (in_window(key)) {
      const detail::Index existing = slots_[slot_of(key)];
      return existing != detail::kNil ? existing : place(key, make());
    }
    return overflow_find_or_insert(key, make);
  }

  /// Removes `key`. Precondition: `key` is present.
  void erase(LevelKey key) noexcept {
    if (ring_count_ > 0 && in_window(key)) {
      unplace(key);
      if (ring_count_ == 0) {
        if (!overflow_.empty()) {
          refill_from_overflow();
        }
        return;
      }
      if (key == best_) {
        best_ = *next_occupied(key + 1, base_ + window_);
        if (best_ - base_ >= recenter_distance_) {
          shift_up(best_ - headroom_);
        }
      }
      return;
    }
    overflow_erase(key);
  }

  /// The best level, or `detail::kNil` if the index is empty.
  [[nodiscard]] detail::Index best() const noexcept {
    return ring_count_ == 0 ? detail::kNil : slots_[slot_of(best_)];
  }

  /// The key of the best level. Precondition: `!empty()`.
  [[nodiscard]] LevelKey best_key() const noexcept {
    assert(ring_count_ > 0);
    return best_;
  }

  /// Calls `visit(key, level)` from the best level to the worst until it
  /// returns false.
  template <class Visit>
  void for_each(Visit&& visit) const {
    if (ring_count_ > 0) {
      const LevelKey end = base_ + window_;
      for (std::optional<LevelKey> key = best_; key; key = next_occupied(*key + 1, end)) {
        if (!visit(*key, slots_[slot_of(*key)])) {
          return;
        }
      }
    }
    for (auto it = overflow_.rbegin(); it != overflow_.rend(); ++it) {
      if (!visit(it->key, it->level)) {
        return;
      }
    }
  }

  /// Number of levels.
  [[nodiscard]] std::size_t size() const noexcept { return ring_count_ + overflow_.size(); }

  /// True if there are no levels.
  [[nodiscard]] bool empty() const noexcept { return ring_count_ == 0; }

  /// First key of the window (meaningful only while `!empty()`).
  [[nodiscard]] LevelKey window_begin() const noexcept { return base_; }

  /// Width of the window in ticks.
  [[nodiscard]] std::uint32_t window_size() const noexcept { return window_; }

  /// Number of levels outside the window.
  [[nodiscard]] std::size_t overflow_size() const noexcept { return overflow_.size(); }

  /// Checks the structural invariants; returns a description of the first
  /// violation, or `std::nullopt`. O(window / 64 + levels).
  [[nodiscard]] std::optional<std::string> check_invariants() const {
    std::size_t bits_set = 0;
    for (const std::uint64_t word : bits_) {
      bits_set += static_cast<std::size_t>(std::popcount(word));
    }
    if (bits_set != ring_count_) {
      return "ladder: occupancy bitmap disagrees with the ring count";
    }
    if (ring_count_ == 0) {
      if (!overflow_.empty()) {
        return "ladder: overflow holds levels while the ring is empty";
      }
      return std::nullopt;
    }
    if (!in_window(best_) || slots_[slot_of(best_)] == detail::kNil) {
      return "ladder: best key is not an occupied window slot";
    }
    if (next_occupied(base_, best_)) {
      return "ladder: an occupied slot is better than the best key";
    }
    for (std::size_t word = 0; word < bits_.size(); ++word) {
      for (std::uint64_t bits = bits_[word]; bits != 0; bits &= bits - 1) {
        const std::size_t slot = word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
        if (slots_[slot] == detail::kNil) {
          return "ladder: bitmap marks an empty slot";
        }
      }
    }
    for (std::size_t i = 0; i < overflow_.size(); ++i) {
      const LevelKey key = overflow_[i].key;
      if (key < base_ + static_cast<LevelKey>(window_)) {
        return "ladder: overflow key inside or below the window";
      }
      if (i > 0 && overflow_[i - 1].key <= key) {
        return "ladder: overflow is not sorted from worst to best";
      }
    }
    return std::nullopt;
  }

 private:
  struct Entry {
    LevelKey key;
    detail::Index level;
  };

  [[nodiscard]] bool in_window(LevelKey key) const noexcept {
    // Unsigned wrap-around turns "base <= key < base + W" into one comparison.
    return static_cast<std::uint64_t>(key) - static_cast<std::uint64_t>(base_) < window_;
  }

  [[nodiscard]] std::size_t slot_of(LevelKey key) const noexcept {
    return static_cast<std::size_t>(static_cast<std::uint64_t>(key) & (window_ - 1));
  }

  static constexpr std::uint64_t bit_of(std::size_t slot) noexcept {
    return std::uint64_t{1} << (slot & 63U);
  }

  detail::Index place(LevelKey key, detail::Index level) noexcept {
    const std::size_t slot = slot_of(key);
    assert(slots_[slot] == detail::kNil);
    slots_[slot] = level;
    bits_[slot >> 6U] |= bit_of(slot);
    if (ring_count_ == 0 || key < best_) {
      best_ = key;
    }
    ++ring_count_;
    return level;
  }

  void unplace(LevelKey key) noexcept {
    const std::size_t slot = slot_of(key);
    assert(slots_[slot] != detail::kNil);
    slots_[slot] = detail::kNil;
    bits_[slot >> 6U] &= ~bit_of(slot);
    --ring_count_;
  }

  /// Smallest occupied key in `[from, end)`, where `end - from <= W`.
  [[nodiscard]] std::optional<LevelKey> next_occupied(LevelKey from, LevelKey end) const noexcept {
    while (from < end) {
      const std::size_t slot = slot_of(from);
      const auto offset = static_cast<unsigned>(slot & 63U);
      const LevelKey span = std::min<LevelKey>(64 - offset, end - from);
      std::uint64_t word = bits_[slot >> 6U] >> offset;
      if (span < 64) {
        word &= (std::uint64_t{1} << static_cast<unsigned>(span)) - 1;
      }
      if (word != 0) {
        return from + std::countr_zero(word);
      }
      from += span;
    }
    return std::nullopt;
  }

  /// Moves the window down to `new_base` (< base_), evicting ring keys that
  /// no longer fit into the overflow.
  void shift_down(LevelKey new_base) {
    const LevelKey old_end = base_ + window_;
    const LevelKey new_end = new_base + window_;
    const std::size_t first_evicted = overflow_.size();
    for (std::optional<LevelKey> key = next_occupied(std::max(new_end, base_), old_end); key;
         key = next_occupied(*key + 1, old_end)) {
      overflow_.push_back(Entry{*key, slots_[slot_of(*key)]});
      unplace(*key);
    }
    // Evicted keys were collected best-first and are all better than the
    // existing overflow; reverse them to keep the vector sorted worst-first.
    std::reverse(overflow_.begin() + static_cast<std::ptrdiff_t>(first_evicted), overflow_.end());
    base_ = new_base;
  }

  /// Moves the window up to `new_base`. Precondition: no ring key is below
  /// `new_base`, so the slots that change meaning are all empty.
  void shift_up(LevelKey new_base) noexcept {
    base_ = new_base;
    pull_from_overflow();
  }

  /// Re-centres an empty ring on the best overflow level.
  void refill_from_overflow() noexcept {
    base_ = overflow_.back().key - headroom_;
    pull_from_overflow();
  }

  void pull_from_overflow() noexcept {
    while (!overflow_.empty() && in_window(overflow_.back().key)) {
      place(overflow_.back().key, overflow_.back().level);
      overflow_.pop_back();
    }
  }

  [[nodiscard]] std::vector<Entry>::const_iterator overflow_lower_bound(LevelKey key) const {
    // First entry whose key is <= `key` (the vector is sorted descending).
    return std::lower_bound(overflow_.begin(), overflow_.end(), key,
                            [](const Entry& entry, LevelKey k) { return entry.key > k; });
  }

  [[nodiscard]] detail::Index overflow_find(LevelKey key) const noexcept {
    const auto it = overflow_lower_bound(key);
    return it != overflow_.end() && it->key == key ? it->level : detail::kNil;
  }

  template <class Make>
  detail::Index overflow_find_or_insert(LevelKey key, Make& make) {
    const auto it = overflow_lower_bound(key);
    if (it != overflow_.end() && it->key == key) {
      return it->level;
    }
    const detail::Index level = make();
    overflow_.insert(it, Entry{key, level});
    return level;
  }

  void overflow_erase(LevelKey key) noexcept {
    const auto it = overflow_lower_bound(key);
    assert(it != overflow_.end() && it->key == key);
    overflow_.erase(it);
  }

  std::uint32_t window_;
  LevelKey headroom_;
  LevelKey recenter_distance_;
  LevelKey base_ = 0;
  LevelKey best_ = 0;
  std::size_t ring_count_ = 0;
  std::vector<detail::Index> slots_;
  std::vector<std::uint64_t> bits_;
  std::vector<Entry> overflow_;  // Sorted by key, descending: best at the back.
};

}  // namespace ob
