// SPDX-License-Identifier: MIT
/// @file
/// Index-based object pool: the storage behind orders and price levels.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace ob::detail {

/// Handle of a pooled object. 32 bits instead of a 64-bit pointer halves the
/// size of every intrusive link.
using Index = std::uint32_t;

/// The null handle.
inline constexpr Index kNil = std::numeric_limits<Index>::max();

/// A pool of `T` objects addressed by `Index`.
///
/// Objects live contiguously in one vector and freed slots are recycled LIFO,
/// so recently freed (cache-hot) slots are reused first. `allocate` grows the
/// storage geometrically when no slot is free; once the pool has reached its
/// high-water mark (or after `reserve`), neither `allocate` nor `release`
/// touches the heap. Growth moves the objects, so callers hold indices, never
/// references, across an `allocate`.
template <class T>
  requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class Pool {
 public:
  /// Makes room for `capacity` objects without further allocation.
  void reserve(std::size_t capacity) {
    items_.reserve(capacity);
    free_.reserve(items_.capacity());
  }

  /// Returns the index of an unused slot. Its contents are unspecified; the
  /// caller initialises it. Precondition: fewer than `kNil` objects are live.
  [[nodiscard]] Index allocate() {
    if (!free_.empty()) {
      const Index index = free_.back();
      free_.pop_back();
      return index;
    }
    assert(items_.size() < kNil);
    if (items_.size() == items_.capacity()) {
      reserve(items_.empty() ? 16 : items_.size() * 2);
    }
    items_.emplace_back();
    return static_cast<Index>(items_.size() - 1);
  }

  /// Returns `index` to the pool. Never allocates: the free list always has
  /// capacity for every slot.
  void release(Index index) noexcept {
    assert(index < items_.size());
    free_.push_back(index);
  }

  /// The object at `index`.
  [[nodiscard]] T& operator[](Index index) noexcept {
    assert(index < items_.size());
    return items_[index];
  }

  /// The object at `index`.
  [[nodiscard]] const T& operator[](Index index) const noexcept {
    assert(index < items_.size());
    return items_[index];
  }

  /// Number of allocated (live) objects.
  [[nodiscard]] std::size_t live() const noexcept { return items_.size() - free_.size(); }

  /// Number of slots the pool can hold without allocating.
  [[nodiscard]] std::size_t capacity() const noexcept { return items_.capacity(); }

 private:
  std::vector<T> items_;
  std::vector<Index> free_;
};

}  // namespace ob::detail
