// SPDX-License-Identifier: MIT
// Level indexes: behaviour shared by LadderLevels and MapLevels, a randomized
// comparison against a std::map model, and the ladder's window mechanics.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/detail/pool.hpp>
#include <orderbook/ladder_levels.hpp>
#include <orderbook/level_index.hpp>
#include <orderbook/map_levels.hpp>
#include <orderbook/random.hpp>
#include <orderbook/types.hpp>

namespace ob {
namespace {

using detail::Index;
using detail::kNil;

template <class L>
std::vector<LevelKey> keys_of(const L& index) {
  std::vector<LevelKey> keys;
  index.for_each([&](LevelKey key, Index /*level*/) {
    keys.push_back(key);
    return true;
  });
  return keys;
}

template <class L>
class LevelIndexTest : public ::testing::Test {};

using Indexes = ::testing::Types<LadderLevels, MapLevels>;
TYPED_TEST_SUITE(LevelIndexTest, Indexes);

TYPED_TEST(LevelIndexTest, StartsEmpty) {
  const TypeParam index(64);
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.size(), 0U);
  EXPECT_EQ(index.best(), kNil);
  EXPECT_EQ(index.find(0), kNil);
  EXPECT_TRUE(keys_of(index).empty());
  EXPECT_EQ(index.check_invariants(), std::nullopt);
}

TYPED_TEST(LevelIndexTest, FindOrInsertCreatesOnlyMissingLevels) {
  TypeParam index(64);
  Index next = 0;
  int made = 0;
  const auto make = [&] {
    ++made;
    return next++;
  };
  EXPECT_EQ(index.find_or_insert(5, make), 0U);
  EXPECT_EQ(index.find_or_insert(3, make), 1U);
  EXPECT_EQ(index.find_or_insert(9, make), 2U);
  EXPECT_EQ(index.find_or_insert(3, make), 1U);
  EXPECT_EQ(made, 3);
  EXPECT_EQ(index.size(), 3U);
  EXPECT_EQ(index.best(), 1U);
  EXPECT_EQ(index.best_key(), 3);
  EXPECT_EQ(index.find(9), 2U);
  EXPECT_EQ(index.find(4), kNil);
  EXPECT_EQ(keys_of(index), (std::vector<LevelKey>{3, 5, 9}));
}

TYPED_TEST(LevelIndexTest, EraseUpdatesTheBestLevel) {
  TypeParam index(64);
  Index next = 0;
  for (const LevelKey key : {7, 2, 30, 4}) {
    index.find_or_insert(key, [&] { return next++; });
  }
  index.erase(2);
  EXPECT_EQ(index.best_key(), 4);
  index.erase(30);
  EXPECT_EQ(keys_of(index), (std::vector<LevelKey>{4, 7}));
  index.erase(4);
  index.erase(7);
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.best(), kNil);
  EXPECT_EQ(index.check_invariants(), std::nullopt);
}

TYPED_TEST(LevelIndexTest, ForEachStopsWhenVisitReturnsFalse) {
  TypeParam index(64);
  Index next = 0;
  for (const LevelKey key : {1, 2, 3, 500, 501}) {
    index.find_or_insert(key, [&] { return next++; });
  }
  std::vector<LevelKey> seen;
  index.for_each([&](LevelKey key, Index /*level*/) {
    seen.push_back(key);
    return seen.size() < 4;
  });
  EXPECT_EQ(seen, (std::vector<LevelKey>{1, 2, 3, 500}));
}

/// Applies random inserts and erases to `index` and to a std::map model and
/// requires them to agree after every operation.
template <class L>
void run_against_model(std::uint32_t window, std::uint64_t seed, int operations) {
  L index(window);
  std::map<LevelKey, Index> model;
  Rng rng(seed);
  Index next = 0;
  LevelKey center = 0;
  for (int op = 0; op < operations; ++op) {
    const std::uint64_t kind = rng.below(100);
    if (kind < 2) {
      center += rng.between(-5 * static_cast<std::int64_t>(window), 5 * window);
    } else if (kind < 55 || model.empty()) {
      LevelKey key = center + rng.between(-40, 40);
      if (kind < 4) {
        key = rng.chance(1, 2) ? kMinPrice.value() : kMaxPrice.value();
      } else if (kind < 10) {
        key = center + rng.between(-1'000'000, 1'000'000);
      }
      const auto found = model.find(key);
      const Index expected = found == model.end() ? next : found->second;
      ASSERT_EQ(index.find_or_insert(key, [&] { return next++; }), expected) << "op " << op;
      model.emplace(key, expected);
    } else if (kind < 75) {
      // Erase the best level, as matching does.
      index.erase(model.begin()->first);
      model.erase(model.begin());
    } else {
      auto it = model.begin();
      std::advance(it, static_cast<std::ptrdiff_t>(rng.below(model.size())));
      index.erase(it->first);
      model.erase(it);
    }

    ASSERT_EQ(index.size(), model.size()) << "op " << op;
    if (model.empty()) {
      ASSERT_EQ(index.best(), kNil);
    } else {
      ASSERT_EQ(index.best_key(), model.begin()->first) << "op " << op;
      ASSERT_EQ(index.best(), model.begin()->second) << "op " << op;
    }
    const LevelKey probe = center + rng.between(-60, 60);
    const auto found = model.find(probe);
    ASSERT_EQ(index.find(probe), found == model.end() ? kNil : found->second) << "op " << op;
    ASSERT_EQ(index.check_invariants(), std::nullopt) << "op " << op;
    if (op % 32 == 0) {
      std::vector<LevelKey> expected;
      for (const auto& entry : model) {
        expected.push_back(entry.first);
      }
      ASSERT_EQ(keys_of(index), expected) << "op " << op;
    }
  }
}

TYPED_TEST(LevelIndexTest, AgreesWithAStdMapModel) {
  for (const std::uint32_t window : {64U, 256U, 4096U}) {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
      SCOPED_TRACE(testing::Message() << "window " << window << " seed " << seed);
      run_against_model<TypeParam>(window, seed, 20'000);
    }
  }
}

// --- Ladder window mechanics ---------------------------------------------------

TEST(LadderLevelsTest, WindowIsAPowerOfTwoWithinBounds) {
  EXPECT_EQ(LadderLevels(100).window_size(), 128U);
  EXPECT_EQ(LadderLevels(1).window_size(), LadderLevels::kMinWindow);
  EXPECT_EQ(LadderLevels(4096).window_size(), 4096U);
  EXPECT_EQ(LadderLevels(0xffffffffU).window_size(), LadderLevels::kMaxWindow);
}

TEST(LadderLevelsTest, KeysPastTheWindowGoToTheOverflow) {
  LadderLevels index(64);  // Headroom 8: the first key sits 8 ticks above the base.
  Index next = 0;
  const auto make = [&] { return next++; };
  index.find_or_insert(0, make);
  EXPECT_EQ(index.window_begin(), -8);
  index.find_or_insert(55, make);  // Last key of [-8, 56).
  EXPECT_EQ(index.overflow_size(), 0U);
  index.find_or_insert(56, make);
  index.find_or_insert(1000, make);
  EXPECT_EQ(index.overflow_size(), 2U);
  EXPECT_EQ(index.find(56), 2U);
  EXPECT_EQ(index.find(1000), 3U);
  EXPECT_EQ(keys_of(index), (std::vector<LevelKey>{0, 55, 56, 1000}));
  EXPECT_EQ(index.check_invariants(), std::nullopt);
}

TEST(LadderLevelsTest, BetterKeyShiftsTheWindowDown) {
  LadderLevels index(64);
  Index next = 0;
  const auto make = [&] { return next++; };
  index.find_or_insert(0, make);
  index.find_or_insert(40, make);
  index.find_or_insert(-30, make);  // Window becomes [-38, 26): 40 is evicted.
  EXPECT_EQ(index.window_begin(), -38);
  EXPECT_EQ(index.overflow_size(), 1U);
  index.find_or_insert(-100, make);  // Window becomes [-108, -44): everything else is evicted.
  EXPECT_EQ(index.window_begin(), -108);
  EXPECT_EQ(index.overflow_size(), 3U);
  EXPECT_EQ(index.best_key(), -100);
  EXPECT_EQ(keys_of(index), (std::vector<LevelKey>{-100, -30, 0, 40}));
  EXPECT_EQ(index.check_invariants(), std::nullopt);
}

TEST(LadderLevelsTest, EmptyingTheRingRecentresOnTheOverflow) {
  LadderLevels index(64);
  Index next = 0;
  const auto make = [&] { return next++; };
  index.find_or_insert(-100, make);
  index.find_or_insert(0, make);
  index.find_or_insert(40, make);
  ASSERT_EQ(index.overflow_size(), 2U);
  index.erase(-100);
  EXPECT_EQ(index.window_begin(), -8);
  EXPECT_EQ(index.overflow_size(), 0U);
  EXPECT_EQ(index.best_key(), 0);
  EXPECT_EQ(index.check_invariants(), std::nullopt);
}

TEST(LadderLevelsTest, BestDriftingAwayShiftsTheWindowUp) {
  LadderLevels index(64);
  Index next = 0;
  const auto make = [&] { return next++; };
  for (LevelKey key = 0; key <= 100; key += 10) {
    index.find_or_insert(key, make);
  }
  ASSERT_EQ(index.window_begin(), -8);
  ASSERT_EQ(index.overflow_size(), 5U);  // 60 ... 100
  index.erase(0);
  index.erase(10);
  EXPECT_EQ(index.window_begin(), -8);  // Best 20 is 28 ticks above the base.
  index.erase(20);                      // Best 30 is 38 >= 32 ticks above: shift.
  EXPECT_EQ(index.window_begin(), 22);
  EXPECT_EQ(index.overflow_size(), 2U);  // [22, 86) now holds 30 ... 80; 90 and 100 stay out.
  EXPECT_EQ(keys_of(index), (std::vector<LevelKey>{30, 40, 50, 60, 70, 80, 90, 100}));
  EXPECT_EQ(index.check_invariants(), std::nullopt);
}

}  // namespace
}  // namespace ob
