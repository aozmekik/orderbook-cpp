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
    OrderIdMap map(8);
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

}  // namespace
}  // namespace ob::detail
