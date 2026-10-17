// SPDX-License-Identifier: MIT
/// @file
/// Open-addressing hash map from order id to order handle.

#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <orderbook/detail/pool.hpp>

namespace ob::detail {

/// Maps non-zero 64-bit keys to `Index` values.
///
/// Linear probing over a power-of-two table kept at most half full, with
/// backward-shift deletion (Knuth, TAOCP vol. 3, 6.4, Algorithm R): erasing
/// moves later members of the cluster back instead of leaving tombstones, so
/// probe lengths do not degrade under the insert/erase churn of an order book.
/// Key 0 marks an empty slot, which is why order id 0 is reserved.
///
/// Keys are scrambled with the MurmurHash3 64-bit finalizer because order ids
/// are usually sequential, and sequential keys would form long clusters under
/// an identity hash.
class OrderIdMap {
 public:
  /// Creates a map that holds `capacity` keys before it has to grow.
  explicit OrderIdMap(std::size_t capacity = 16) { rehash(table_size_for(capacity)); }

  /// The value stored for `key`, or `kNil`.
  [[nodiscard]] Index find(std::uint64_t key) const noexcept {
    assert(key != 0);
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& slot = slots_[i];
      if (slot.key == key) {
        return slot.value;
      }
      if (slot.key == 0) {
        return kNil;
      }
    }
  }

  /// Inserts `key`. Precondition: `key` is non-zero and not present.
  /// Allocates only when the table must grow.
  void insert(std::uint64_t key, Index value) {
    assert(key != 0);
    assert(find(key) == kNil);
    if ((size_ + 1) * 2 > slots_.size()) {
      rehash(slots_.size() * 2);
    }
    place(key, value);
    ++size_;
  }

  /// Removes `key`; returns false if it was not present.
  bool erase(std::uint64_t key) noexcept {
    assert(key != 0);
    std::size_t hole = home(key);
    while (slots_[hole].key != key) {
      if (slots_[hole].key == 0) {
        return false;
      }
      hole = (hole + 1) & mask_;
    }
    // Backward shift: walk the rest of the cluster and move back every entry
    // whose home is not between the hole and its current slot.
    for (std::size_t i = (hole + 1) & mask_; slots_[i].key != 0; i = (i + 1) & mask_) {
      const std::size_t distance_from_home = (i - home(slots_[i].key)) & mask_;
      const std::size_t distance_from_hole = (i - hole) & mask_;
      if (distance_from_home >= distance_from_hole) {
        slots_[hole] = slots_[i];
        hole = i;
      }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
  }

  /// Grows the table so that `capacity` keys fit without further allocation.
  void reserve(std::size_t capacity) {
    const std::size_t wanted = table_size_for(capacity);
    if (wanted > slots_.size()) {
      rehash(wanted);
    }
  }

  /// Number of keys.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// Number of keys that fit without growing.
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size() / 2; }

 private:
  struct Slot {
    std::uint64_t key = 0;
    Index value = kNil;
  };

  static std::size_t table_size_for(std::size_t capacity) noexcept {
    return std::bit_ceil(capacity < 8 ? std::size_t{16} : capacity * 2);
  }

  /// MurmurHash3 fmix64 (Austin Appleby, public domain).
  static std::uint64_t mix(std::uint64_t key) noexcept {
    key ^= key >> 33U;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33U;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33U;
    return key;
  }

  [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>(mix(key)) & mask_;
  }

  void place(std::uint64_t key, Index value) noexcept {
    std::size_t i = home(key);
    while (slots_[i].key != 0) {
      i = (i + 1) & mask_;
    }
    slots_[i] = Slot{key, value};
  }

  void rehash(std::size_t table_size) {
    std::vector<Slot> old(table_size);
    old.swap(slots_);
    mask_ = table_size - 1;
    for (const Slot& slot : old) {
      if (slot.key != 0) {
        place(slot.key, slot.value);
      }
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  // namespace ob::detail
