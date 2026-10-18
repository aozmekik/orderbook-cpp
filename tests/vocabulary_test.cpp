// SPDX-License-Identifier: MIT
// Vocabulary types, validation helpers, ob::expected and the RNG.

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/expected.hpp>
#include <orderbook/level_index.hpp>
#include <orderbook/random.hpp>
#include <orderbook/types.hpp>

namespace ob {
namespace {

// A price and a quantity must not mix.
static_assert(!std::is_convertible_v<Qty, Price>);
static_assert(!std::is_convertible_v<std::int64_t, Price>);
static_assert(!std::is_constructible_v<Price, Qty>);

TEST(StrongIntTest, ArithmeticAndComparison) {
  Qty qty{10};
  qty += Qty{5};
  qty -= Qty{3};
  EXPECT_EQ(qty, Qty{12});
  EXPECT_EQ(Qty{4} + Qty{6}, Qty{10});
  EXPECT_EQ(Qty{4} - Qty{6}, Qty{-2});
  EXPECT_LT(Price{-1}, Price{0});
  EXPECT_EQ(Price{}, Price{0});
}

TEST(TypesTest, NamesAreStable) {
  EXPECT_EQ(to_string(Side::buy), "buy");
  EXPECT_EQ(to_string(Side::sell), "sell");
  EXPECT_EQ(to_string(OrderType::post_only), "post_only");
  EXPECT_EQ(to_string(StpPolicy::cancel_oldest), "cancel_oldest");
  EXPECT_EQ(to_string(Request::new_order), "new");
  EXPECT_EQ(to_string(RejectReason::post_only_would_cross), "post_only_would_cross");
  EXPECT_EQ(to_string(CancelReason::fok_unfillable), "fok_unfillable");
  EXPECT_EQ(opposite(Side::buy), Side::sell);
  EXPECT_EQ(opposite(Side::sell), Side::buy);
}

TEST(TypesTest, EveryEnumeratorHasADistinctName) {
  std::set<std::string_view> reasons;
  for (int i = 0; i <= static_cast<int>(RejectReason::book_full); ++i) {
    reasons.insert(to_string(static_cast<RejectReason>(i)));
  }
  EXPECT_EQ(reasons.size(), 8U);
  EXPECT_FALSE(reasons.contains("?"));
  std::set<std::string_view> cancels;
  for (int i = 0; i <= static_cast<int>(CancelReason::self_trade); ++i) {
    cancels.insert(to_string(static_cast<CancelReason>(i)));
  }
  EXPECT_EQ(cancels.size(), 4U);
  EXPECT_FALSE(cancels.contains("?"));
}

TEST(TypesTest, LevelKeysOrderBothSidesBestFirst) {
  // Higher bids and lower asks are better, i.e. have smaller keys.
  EXPECT_LT(level_key(Side::buy, Price{101}), level_key(Side::buy, Price{100}));
  EXPECT_LT(level_key(Side::sell, Price{100}), level_key(Side::sell, Price{101}));
  for (const Price price : {kMinPrice, Price{-3}, Price{0}, Price{7}, kMaxPrice}) {
    EXPECT_EQ(level_price(Side::buy, level_key(Side::buy, price)), price);
    EXPECT_EQ(level_price(Side::sell, level_key(Side::sell, price)), price);
  }
}

TEST(ValidationTest, ChecksIdThenQuantityThenPrice) {
  NewOrder order{SymbolId{0}, OrderId{0},       OwnerId{0},
                 Side::buy,   OrderType::limit, Price{kMaxPrice.value() + 1},
                 Qty{0}};
  EXPECT_EQ(validate(order), RejectReason::invalid_order_id);
  order.id = OrderId{1};
  EXPECT_EQ(validate(order), RejectReason::invalid_quantity);
  order.qty = Qty{1};
  EXPECT_EQ(validate(order), RejectReason::invalid_price);
  order.type = OrderType::market;
  EXPECT_EQ(validate(order), std::nullopt);
  EXPECT_TRUE(is_valid_qty(kMaxOrderQty));
  EXPECT_FALSE(is_valid_qty(kMaxOrderQty + Qty{1}));
  EXPECT_TRUE(is_valid_price(kMinPrice));
  EXPECT_FALSE(is_valid_price(kMinPrice - Price{1}));
  EXPECT_TRUE(can_rest(OrderType::post_only));
  EXPECT_FALSE(can_rest(OrderType::ioc));
}

// The fallback is tested directly so that it is covered even when the
// standard library provides std::expected.
TEST(ExpectedTest, FallbackHoldsAValueOrAnError) {
  using Result = detail::ExpectedFallback<std::string, int>;
  const Result value = std::string("ok");
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(static_cast<bool>(value));
  EXPECT_EQ(*value, "ok");
  EXPECT_EQ(value->size(), 2U);

  const Result error = detail::UnexpectedFallback<int>(42);
  ASSERT_FALSE(error.has_value());
  EXPECT_EQ(error.error(), 42);

  Result moved = std::string("moved");
  const std::string out = *std::move(moved);
  EXPECT_EQ(out, "moved");
}

TEST(ExpectedTest, AliasMatchesTheStandardLibrary) {
#if OB_HAS_STD_EXPECTED
  static_assert(std::is_same_v<expected<int, char>, std::expected<int, char>>);
#else
  static_assert(std::is_same_v<expected<int, char>, detail::ExpectedFallback<int, char>>);
#endif
  const auto failing = []() -> expected<int, std::string> {
    return make_unexpected(std::string("bad"));
  };
  const auto working = []() -> expected<int, std::string> { return 7; };
  EXPECT_EQ(failing().error(), "bad");
  EXPECT_EQ(*working(), 7);
}

TEST(RngTest, IsDeterministicForASeed) {
  Rng a(123);
  Rng b(123);
  Rng c(124);
  bool differs = false;
  for (int i = 0; i < 100; ++i) {
    const std::uint64_t x = a.next();
    EXPECT_EQ(x, b.next());
    differs = differs || x != c.next();
  }
  EXPECT_TRUE(differs);
}

TEST(RngTest, ProducesAPinnedSequence) {
  // Pins the generator so that workloads stay reproducible across releases.
  Rng rng(1);
  EXPECT_EQ(rng.next(), 0xb3f2af6d0fc710c5ULL);
  EXPECT_EQ(rng.next(), 0x853b559647364ceaULL);
}

TEST(RngTest, BoundedValuesStayInRangeAndCoverIt) {
  Rng rng(7);
  std::set<std::uint64_t> seen;
  for (int i = 0; i < 10'000; ++i) {
    const std::uint64_t value = rng.below(10);
    ASSERT_LT(value, 10U);
    seen.insert(value);
    const std::int64_t signed_value = rng.between(-3, 3);
    ASSERT_GE(signed_value, -3);
    ASSERT_LE(signed_value, 3);
  }
  EXPECT_EQ(seen.size(), 10U);
  EXPECT_EQ(rng.between(5, 5), 5);
}

}  // namespace
}  // namespace ob
