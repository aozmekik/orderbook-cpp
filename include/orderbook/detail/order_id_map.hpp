// SPDX-License-Identifier: MIT
/// @file
/// Open-addressing hash map from order id to order handle.

#pragma once

#include <algorithm>
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
/// The home slot of a key is `(fmix64(key) * m) >> (64 - log2(table size))`:
/// the MurmurHash3 finalizer spreads sequential ids, and multiply-shift
/// hashing with an odd multiplier `m` derived from a per-map seed
/// (Dietzfelbinger et al., 1997) makes any two distinct keys share a home
/// slot with probability at most 2 / table size over the choice of seed.
/// Keys are chosen by clients and fmix64 is public and invertible, so
/// without the seed a client could pick ids that all land in one slot and
/// turn every operation into a scan of the whole table. With a secret seed
/// such a set of ids cannot be computed in advance. This is not a
/// cryptographic guarantee: a client that can time many requests could in
/// principle learn about the seed, so a venue facing untrusted clients
/// should still map their ids to its own at the gateway.
class OrderIdMap {
 public:
  /// Creates a map that holds `capacity` keys before it has to grow, hashing
  /// with `seed`. Equal seeds give equal table layouts; the layout never
  /// affects what the map returns.
  explicit OrderIdMap(std::size_t capacity = 16, std::uint64_t seed = 0)
      : multiplier_(odd_multiplier(seed)) {
    rehash(table_size_for(capacity));
  }

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

  /// The 64-bit hash of `key` under `seed`. The home slot of `key` in a
  /// table of 2^b slots is the top b bits.
  [[nodiscard]] static std::uint64_t hash(std::uint64_t key, std::uint64_t seed) noexcept {
    return mix(key) * odd_multiplier(seed);
  }

  /// The longest distance of a key from its home slot: the worst probe
  /// sequence of a lookup, minus one. O(table size); for tests.
  [[nodiscard]] std::size_t longest_probe() const noexcept {
    std::size_t longest = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].key != 0) {
        longest = std::max(longest, (i - home(slots_[i].key)) & mask_);
      }
    }
    return longest;
  }

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

  /// An odd multiplier drawn from `seed` with the SplitMix64 output function
  /// (Steele, Lea and Flood, 2014), so that nearby seeds give unrelated
  /// multipliers.
  static std::uint64_t odd_multiplier(std::uint64_t seed) noexcept {
    std::uint64_t z = seed + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return (z ^ (z >> 31U)) | 1U;
  }

  [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((mix(key) * multiplier_) >> shift_);
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
    shift_ = static_cast<unsigned>(64 - std::countr_zero(table_size));
    for (const Slot& slot : old) {
      if (slot.key != 0) {
        place(slot.key, slot.value);
      }
    }
  }

  std::vector<Slot> slots_;
  std::uint64_t multiplier_;
  std::size_t mask_ = 0;
  unsigned shift_ = 64;
  std::size_t size_ = 0;
};

/// A seed for `OrderIdMap` from `std::random_device`. Allocates nothing but
/// may be slow; call it when a book is created, not per request.
[[nodiscard]] std::uint64_t random_hash_seed();

}  // namespace ob::detail
