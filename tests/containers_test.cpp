// SPDX-License-Identifier: MIT
// The building blocks: object pool and open-addressing id map.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/detail/order_id_map.hpp>
#include <orderbook/detail/pool.hpp>
#include <orderbook/random.hpp>

namespace ob::detail {
namespace {

struct Item {
  int value = 0;
};

TEST(PoolTest, AllocatesDistinctSlotsAndRecyclesTheMostRecentlyReleased) {
  Pool<Item> pool;
  const Index a = pool.allocate();
  const Index b = pool.allocate();
  const Index c = pool.allocate();
  EXPECT_NE(a, b);
  EXPECT_NE(b, c);
  EXPECT_EQ(pool.live(), 3U);
  pool.release(b);
  pool.release(a);
  EXPECT_EQ(pool.live(), 1U);
  EXPECT_EQ(pool.allocate(), a);  // LIFO
  EXPECT_EQ(pool.allocate(), b);
  EXPECT_EQ(pool.live(), 3U);
}

TEST(PoolTest, KeepsValuesAcrossGrowth) {
  Pool<Item> pool;
  std::vector<Index> indices;
  for (int i = 0; i < 1000; ++i) {
    const Index index = pool.allocate();
    pool[index].value = i;
    indices.push_back(index);
  }
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(pool[indices[static_cast<std::size_t>(i)]].value, i);
  }
  EXPECT_GE(pool.capacity(), 1000U);
}

TEST(PoolTest, ReserveMakesRoomUpFront) {
  Pool<Item> pool;
  pool.reserve(500);
  const std::size_t capacity = pool.capacity();
  EXPECT_GE(capacity, 500U);
  for (int i = 0; i < 500; ++i) {
    static_cast<void>(pool.allocate());
  }
  EXPECT_EQ(pool.capacity(), capacity);
}

TEST(OrderIdMapTest, InsertFindErase) {
  OrderIdMap map;
  EXPECT_EQ(map.find(42), kNil);
  map.insert(42, 7);
  map.insert(43, 8);
  EXPECT_EQ(map.find(42), 7U);
  EXPECT_EQ(map.find(43), 8U);
  EXPECT_EQ(map.size(), 2U);
  EXPECT_TRUE(map.erase(42));
  EXPECT_FALSE(map.erase(42));
  EXPECT_EQ(map.find(42), kNil);
  EXPECT_EQ(map.find(43), 8U);
  EXPECT_EQ(map.size(), 1U);
}

TEST(OrderIdMapTest, GrowsAndKeepsEveryKey) {
  OrderIdMap map(4);
  for (std::uint64_t key = 1; key <= 10'000; ++key) {
    map.insert(key, static_cast<Index>(key * 3));
  }
  EXPECT_EQ(map.size(), 10'000U);
  EXPECT_GE(map.capacity(), 10'000U);
  for (std::uint64_t key = 1; key <= 10'000; ++key) {
    ASSERT_EQ(map.find(key), static_cast<Index>(key * 3));
  }
}

TEST(OrderIdMapTest, ReserveAvoidsRehashing) {
  OrderIdMap map;
  map.reserve(1000);
  const std::size_t capacity = map.capacity();
  EXPECT_GE(capacity, 1000U);
  for (std::uint64_t key = 1; key <= 1000; ++key) {
    map.insert(key, 0);
  }
  EXPECT_EQ(map.capacity(), capacity);
}

// Backward-shift deletion is the subtle part: compare long random
// insert/erase sequences against std::unordered_map, with a small table so
// that clusters wrap around the end of the array.
TEST(OrderIdMapTest, AgreesWithStdUnorderedMapUnderChurn) {
  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    OrderIdMap map(8, seed);
    std::unordered_map<std::uint64_t, Index> model;
    Rng rng(seed);
    for (int op = 0; op < 200'000; ++op) {
      // Small key space: lots of collisions, re-insertions and misses.
      const std::uint64_t key = 1 + rng.below(seed == 1 ? 64 : 4096);
      if (rng.chance(1, 2)) {
        if (!model.contains(key)) {
          const auto value = static_cast<Index>(rng.below(1'000'000));
          map.insert(key, value);
          model.emplace(key, value);
        }
      } else {
        ASSERT_EQ(map.erase(key), model.erase(key) == 1) << "seed " << seed << " op " << op;
      }
      const std::uint64_t probe = 1 + rng.below(4096);
      const auto it = model.find(probe);
      ASSERT_EQ(map.find(probe), it == model.end() ? kNil : it->second)
          << "seed " << seed << " op " << op;
      ASSERT_EQ(map.size(), model.size());
    }
    for (const auto& [key, value] : model) {
      ASSERT_EQ(map.find(key), value);
    }
  }
}

/// Inverse of a multiplication by an odd 64-bit number (Newton's method).
std::uint64_t inverse(std::uint64_t odd) {
  std::uint64_t x = odd;  // Correct to 3 bits; each step doubles that.
  for (int i = 0; i < 5; ++i) {
    x *= 2 - (odd * x);
  }
  return x;
}

/// Inverse of the MurmurHash3 fmix64 finalizer: what an attacker who knows
/// the hash would compute. x ^= x >> 33 is its own inverse.
std::uint64_t unmix(std::uint64_t h) {
  h ^= h >> 33U;
  h *= inverse(0xc4ceb9fe1a85ec53ULL);
  h ^= h >> 33U;
  h *= inverse(0xff51afd7ed558ccdULL);
  h ^= h >> 33U;
  return h;
}

/// `count` distinct keys that all hash to the home slot 0 under `seed`.
std::vector<std::uint64_t> colliding_keys(std::uint64_t seed, std::size_t count) {
  const std::uint64_t multiplier = OrderIdMap::hash(unmix(1), seed);
  const std::uint64_t undo = inverse(multiplier);
  std::vector<std::uint64_t> keys;
  for (std::uint64_t i = 1; keys.size() < count; ++i) {
    const std::uint64_t key = unmix(i * undo);  // hash(key, seed) == i: top bits zero.
    if (key != 0) {
      keys.push_back(key);
    }
  }
  return keys;
}

TEST(OrderIdMapTest, UnmixInvertsTheFinalizer) {
  // hash(k, seed) == fmix64(k) * m for an odd m, so hash(unmix(x)) == x * m
  // for every x exactly when unmix inverts fmix64.
  const std::uint64_t m = OrderIdMap::hash(unmix(1), 77);
  EXPECT_EQ(m % 2, 1U);
  for (const std::uint64_t x : {2ULL, 3ULL, 12'345ULL, 0xdead'beefULL, ~0ULL}) {
    EXPECT_EQ(OrderIdMap::hash(unmix(x), 77), x * m);
  }
}

// Keys chosen to collide under one seed fill a single cluster under that
// seed, and spread out under any other: the seed is what defeats a client
// that picks its order ids from the (public, invertible) hash function.
TEST(OrderIdMapTest, KeysChosenForOneSeedDoNotCollideUnderAnother) {
  constexpr std::size_t kKeys = 4096;
  const std::vector<std::uint64_t> keys = colliding_keys(1, kKeys);

  OrderIdMap attacked(kKeys, 1);
  OrderIdMap seeded(kKeys, 2);
  for (const std::uint64_t key : keys) {
    attacked.insert(key, 0);
    seeded.insert(key, 0);
  }
  EXPECT_EQ(attacked.longest_probe(), kKeys - 1);  // One cluster: every lookup scans it.
  EXPECT_LT(seeded.longest_probe(), 16U);
  for (const std::uint64_t key : keys) {
    ASSERT_EQ(seeded.find(key), 0U);
    ASSERT_TRUE(seeded.erase(key));
  }
  EXPECT_EQ(seeded.size(), 0U);
}

TEST(OrderIdMapTest, SequentialKeysStayCloseToHome) {
  for (std::uint64_t seed = 0; seed < 8; ++seed) {
    OrderIdMap map(100'000, seed);
    for (std::uint64_t key = 1; key <= 100'000; ++key) {
      map.insert(key, 0);
    }
    EXPECT_LT(map.longest_probe(), 64U) << "seed " << seed;
  }
}

}  // namespace
}  // namespace ob::detail
