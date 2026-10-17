// SPDX-License-Identifier: MIT
// Matching semantics. Every test runs against all three book implementations
// (ladder, std::map baseline and the reference book) and compares the exact
// event sequence, so the expected output below doubles as a specification.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/reference_book.hpp>
#include <orderbook/types.hpp>

#include "test_support.hpp"

namespace ob {
namespace {

using test::Harness;
using test::kBuy;
using test::kSell;
using test::limit;
using test::Lines;
using test::market;

template <class Book>
class BookTest : public ::testing::Test {
 protected:
  void TearDown() override { EXPECT_EQ(h.violation, std::nullopt); }

  Harness<Book> h;
};

using BookTypes = ::testing::Types<OrderBook, MapOrderBook, ReferenceBook>;
TYPED_TEST_SUITE(BookTest, BookTypes);

// --- Resting and price-time priority -----------------------------------------

TYPED_TEST(BookTest, StartsEmpty) {
  const auto& book = this->h.book;
  EXPECT_EQ(book.best(kBuy), std::nullopt);
  EXPECT_EQ(book.best(kSell), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.level_count(kBuy), 0U);
  EXPECT_EQ(book.level_count(kSell), 0U);
  EXPECT_TRUE(this->h.depth(kBuy).empty());
  EXPECT_EQ(book.find(OrderId{1}), std::nullopt);
  EXPECT_EQ(book.check_invariants(), std::nullopt);
}

TYPED_TEST(BookTest, LimitOrderRests) {
  auto& h = this->h;
  EXPECT_EQ(h.submit(limit(1, kBuy, 100, 10, 7)),
            (Lines{"accepted X id=1 owner=7 side=buy type=limit price=100 qty=10",
                   "delta X bid price=100 qty=10 orders=1"}));
  EXPECT_EQ(h.book.best(kBuy), (LevelView{Price{100}, Qty{10}, 1}));
  EXPECT_EQ(h.book.best(kSell), std::nullopt);
  EXPECT_EQ(h.book.find(OrderId{1}),
            (OrderView{OrderId{1}, OwnerId{7}, kBuy, OrderType::limit, Price{100}, Qty{10}}));
  EXPECT_EQ(h.book.order_count(), 1U);
}

TYPED_TEST(BookTest, OrdersAtOnePriceQueueInArrivalOrder) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(limit(2, kSell, 101, 6));
  EXPECT_EQ(h.submit(limit(3, kSell, 101, 7)),
            (Lines{"accepted X id=3 owner=0 side=sell type=limit price=101 qty=7",
                   "delta X ask price=101 qty=18 orders=3"}));
  EXPECT_EQ(h.queue(kSell, 101), (std::vector<std::uint64_t>{1, 2, 3}));
  EXPECT_EQ(h.book.level_count(kSell), 1U);
}

TYPED_TEST(BookTest, CrossingOrderTradesAtTheMakerPrice) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 10));
  EXPECT_EQ(h.submit(limit(2, kBuy, 105, 4)),
            (Lines{"accepted X id=2 owner=0 side=buy type=limit price=105 qty=4",
                   "fill X taker=2 maker=1 side=buy price=101 qty=4 taker_left=0 maker_left=6",
                   "delta X ask price=101 qty=6 orders=1"}));
  EXPECT_EQ(h.book.best(kBuy), std::nullopt);
}

TYPED_TEST(BookTest, PartialFillRestsTheRemainder) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  EXPECT_EQ(
      h.submit(limit(2, kBuy, 102, 8)),
      (Lines{"accepted X id=2 owner=0 side=buy type=limit price=102 qty=8",
             "fill X taker=2 maker=1 side=buy price=101 qty=5 taker_left=3 maker_left=0",
             "delta X ask price=101 qty=0 orders=0", "delta X bid price=102 qty=3 orders=1"}));
  EXPECT_EQ(h.book.best(kBuy), (LevelView{Price{102}, Qty{3}, 1}));
  EXPECT_EQ(h.book.best(kSell), std::nullopt);
  EXPECT_EQ(h.book.find(OrderId{1}), std::nullopt);
}

TYPED_TEST(BookTest, SweepsLevelsBestPriceFirstAndTimeWithinLevel) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 103, 5));
  h.submit(limit(2, kSell, 101, 5));
  h.submit(limit(3, kSell, 102, 5));
  h.submit(limit(4, kSell, 101, 5));
  EXPECT_EQ(h.submit(limit(5, kBuy, 103, 18)),
            (Lines{"accepted X id=5 owner=0 side=buy type=limit price=103 qty=18",
                   "fill X taker=5 maker=2 side=buy price=101 qty=5 taker_left=13 maker_left=0",
                   "fill X taker=5 maker=4 side=buy price=101 qty=5 taker_left=8 maker_left=0",
                   "delta X ask price=101 qty=0 orders=0",
                   "fill X taker=5 maker=3 side=buy price=102 qty=5 taker_left=3 maker_left=0",
                   "delta X ask price=102 qty=0 orders=0",
                   "fill X taker=5 maker=1 side=buy price=103 qty=3 taker_left=0 maker_left=2",
                   "delta X ask price=103 qty=2 orders=1"}));
}

TYPED_TEST(BookTest, LimitPriceStopsTheSweep) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 5));
  h.submit(limit(2, kBuy, 99, 5));
  h.submit(limit(3, kBuy, 98, 5));
  EXPECT_EQ(h.submit(limit(4, kSell, 99, 20)),
            (Lines{"accepted X id=4 owner=0 side=sell type=limit price=99 qty=20",
                   "fill X taker=4 maker=1 side=sell price=100 qty=5 taker_left=15 maker_left=0",
                   "delta X bid price=100 qty=0 orders=0",
                   "fill X taker=4 maker=2 side=sell price=99 qty=5 taker_left=10 maker_left=0",
                   "delta X bid price=99 qty=0 orders=0", "delta X ask price=99 qty=10 orders=1"}));
  EXPECT_EQ(h.book.best(kBuy), (LevelView{Price{98}, Qty{5}, 1}));
  EXPECT_EQ(h.book.best(kSell), (LevelView{Price{99}, Qty{10}, 1}));
}

TYPED_TEST(BookTest, DepthListsLevelsBestFirst) {
  auto& h = this->h;
  for (std::uint64_t i = 0; i < 5; ++i) {
    h.submit(limit(1 + i, kBuy, 100 - static_cast<std::int64_t>(i * 2), 1));
    h.submit(limit(10 + i, kSell, 105 + static_cast<std::int64_t>(i), 2));
  }
  h.submit(limit(20, kBuy, 98, 4));
  EXPECT_EQ(h.depth(kBuy, 3),
            (std::vector<LevelView>{
                {Price{100}, Qty{1}, 1}, {Price{98}, Qty{5}, 2}, {Price{96}, Qty{1}, 1}}));
  EXPECT_EQ(h.depth(kSell, 2),
            (std::vector<LevelView>{{Price{105}, Qty{2}, 1}, {Price{106}, Qty{2}, 1}}));
  EXPECT_EQ(h.depth(kSell).size(), 5U);
  EXPECT_TRUE(h.depth(kSell, 0).empty());
}

TYPED_TEST(BookTest, NegativePricesMatchLikeAnyOther) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, -50, 3));
  h.submit(limit(2, kBuy, -60, 3));
  h.submit(limit(3, kSell, -40, 3));
  EXPECT_EQ(
      h.submit(limit(4, kSell, -55, 4)),
      (Lines{"accepted X id=4 owner=0 side=sell type=limit price=-55 qty=4",
             "fill X taker=4 maker=1 side=sell price=-50 qty=3 taker_left=1 maker_left=0",
             "delta X bid price=-50 qty=0 orders=0", "delta X ask price=-55 qty=1 orders=1"}));
  EXPECT_EQ(h.book.best(kSell), (LevelView{Price{-55}, Qty{1}, 1}));
}

TYPED_TEST(BookTest, ExtremePricesAndQuantitiesAreSupported) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, kMinPrice.value(), kMaxOrderQty.value()));
  h.submit(limit(2, kBuy, kMinPrice.value(), kMaxOrderQty.value()));
  h.submit(limit(3, kSell, kMaxPrice.value(), 1));
  EXPECT_EQ(h.book.best(kBuy), (LevelView{kMinPrice, Qty{2 * kMaxOrderQty.value()}, 2}));
  EXPECT_EQ(h.book.best(kSell), (LevelView{kMaxPrice, Qty{1}, 1}));
  const Lines events = h.submit(market(4, kSell, 5));
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1],
            "fill X taker=4 maker=1 side=sell price=-4611686018427387904 qty=5 "
            "taker_left=0 maker_left=2147483642");
}

TYPED_TEST(BookTest, LevelsFarApartStayOrdered) {
  // A narrow ladder window forces levels into the overflow and makes the
  // window shift in both directions; the other books must agree.
  Harness<TypeParam> h(BookConfig{.ladder_ticks = 64});
  const std::vector<std::int64_t> prices{1000, 1192, 1640, 680, 1001, 999, 100000, 1063, 1064};
  for (std::size_t i = 0; i < prices.size(); ++i) {
    h.submit(limit(1 + i, kSell, prices[i], 1));
  }
  const Lines events = h.submit(market(100, kBuy, 9));
  std::vector<std::string> fill_prices;
  for (const std::string& line : events) {
    if (line.starts_with("fill")) {
      fill_prices.push_back(
          line.substr(line.find("price="), line.find(" qty") - line.find("price=")));
    }
  }
  EXPECT_EQ(fill_prices, (std::vector<std::string>{"price=680", "price=999", "price=1000",
                                                   "price=1001", "price=1063", "price=1064",
                                                   "price=1192", "price=1640", "price=100000"}));
  EXPECT_EQ(h.book.level_count(kSell), 0U);
  EXPECT_EQ(h.violation, std::nullopt);
}

// --- Market, IOC, FOK, post-only ----------------------------------------------

TYPED_TEST(BookTest, MarketOrderSweepsAndCancelsTheRemainder) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(limit(2, kSell, 102, 5));
  EXPECT_EQ(h.submit(market(3, kBuy, 12)),
            (Lines{"accepted X id=3 owner=0 side=buy type=market price=0 qty=12",
                   "fill X taker=3 maker=1 side=buy price=101 qty=5 taker_left=7 maker_left=0",
                   "delta X ask price=101 qty=0 orders=0",
                   "fill X taker=3 maker=2 side=buy price=102 qty=5 taker_left=2 maker_left=0",
                   "delta X ask price=102 qty=0 orders=0",
                   "cancelled X id=3 qty=2 reason=ioc_remainder"}));
  EXPECT_EQ(h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, MarketOrderIgnoresItsPriceField) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 5));
  NewOrder sell = market(2, kSell, 5);
  sell.price = Price{kMaxPrice.value() + 1};  // Out of range, but unused.
  EXPECT_EQ(h.submit(sell),
            (Lines{"accepted X id=2 owner=0 side=sell type=market price=0 qty=5",
                   "fill X taker=2 maker=1 side=sell price=100 qty=5 taker_left=0 maker_left=0",
                   "delta X bid price=100 qty=0 orders=0"}));
}

TYPED_TEST(BookTest, MarketOrderOnAnEmptySideIsCancelled) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 5));
  EXPECT_EQ(h.submit(market(2, kBuy, 5)),
            (Lines{"accepted X id=2 owner=0 side=buy type=market price=0 qty=5",
                   "cancelled X id=2 qty=5 reason=ioc_remainder"}));
  EXPECT_EQ(h.book.order_count(), 1U);
}

TYPED_TEST(BookTest, IocNeverRests) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  EXPECT_EQ(h.submit(test::order(2, kBuy, OrderType::ioc, 101, 8)),
            (Lines{"accepted X id=2 owner=0 side=buy type=ioc price=101 qty=8",
                   "fill X taker=2 maker=1 side=buy price=101 qty=5 taker_left=3 maker_left=0",
                   "delta X ask price=101 qty=0 orders=0",
                   "cancelled X id=2 qty=3 reason=ioc_remainder"}));
  EXPECT_EQ(h.submit(test::order(3, kBuy, OrderType::ioc, 99, 8)),
            (Lines{"accepted X id=3 owner=0 side=buy type=ioc price=99 qty=8",
                   "cancelled X id=3 qty=8 reason=ioc_remainder"}));
  EXPECT_EQ(h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, FokFillsCompletelyAcrossLevels) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(limit(2, kSell, 102, 5));
  EXPECT_EQ(h.submit(test::order(3, kBuy, OrderType::fok, 102, 10)),
            (Lines{"accepted X id=3 owner=0 side=buy type=fok price=102 qty=10",
                   "fill X taker=3 maker=1 side=buy price=101 qty=5 taker_left=5 maker_left=0",
                   "delta X ask price=101 qty=0 orders=0",
                   "fill X taker=3 maker=2 side=buy price=102 qty=5 taker_left=0 maker_left=0",
                   "delta X ask price=102 qty=0 orders=0"}));
}

TYPED_TEST(BookTest, FokThatCannotFillLeavesTheBookUntouched) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(limit(2, kSell, 102, 5));
  h.submit(limit(3, kSell, 103, 50));
  const std::vector<LevelView> before = h.depth(kSell);
  // 10 lots are available at or below 102; 11 are needed.
  EXPECT_EQ(h.submit(test::order(4, kBuy, OrderType::fok, 102, 11)),
            (Lines{"accepted X id=4 owner=0 side=buy type=fok price=102 qty=11",
                   "cancelled X id=4 qty=11 reason=fok_unfillable"}));
  EXPECT_EQ(h.depth(kSell), before);
}

TYPED_TEST(BookTest, PostOnlyRestsWhenItDoesNotCross) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  EXPECT_EQ(h.submit(test::order(2, kBuy, OrderType::post_only, 100, 5)),
            (Lines{"accepted X id=2 owner=0 side=buy type=post_only price=100 qty=5",
                   "delta X bid price=100 qty=5 orders=1"}));
  EXPECT_EQ(h.book.find(OrderId{2})->type, OrderType::post_only);
}

TYPED_TEST(BookTest, PostOnlyThatWouldTakeLiquidityIsRejected) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  // Touching the best ask counts as crossing.
  EXPECT_EQ(h.submit(test::order(2, kBuy, OrderType::post_only, 101, 5)),
            (Lines{"rejected X id=2 request=new reason=post_only_would_cross"}));
  EXPECT_EQ(h.submit(test::order(3, kBuy, OrderType::post_only, 150, 5)),
            (Lines{"rejected X id=3 request=new reason=post_only_would_cross"}));
  EXPECT_EQ(h.book.order_count(), 1U);
  EXPECT_EQ(h.book.best(kSell), (LevelView{Price{101}, Qty{5}, 1}));
}

// --- Cancel ----------------------------------------------------------------------

TYPED_TEST(BookTest, CancelRemovesTheOrder) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 10));
  h.submit(limit(2, kBuy, 100, 5));
  EXPECT_EQ(h.cancel(test::cancel(1)), (Lines{"cancelled X id=1 qty=10 reason=requested",
                                              "delta X bid price=100 qty=5 orders=1"}));
  EXPECT_EQ(h.cancel(test::cancel(2)), (Lines{"cancelled X id=2 qty=5 reason=requested",
                                              "delta X bid price=100 qty=0 orders=0"}));
  EXPECT_EQ(h.book.best(kBuy), std::nullopt);
  EXPECT_EQ(h.book.level_count(kBuy), 0U);
}

TYPED_TEST(BookTest, CancelOfAnUnknownOrderIsRejected) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  EXPECT_EQ(h.cancel(test::cancel(9)),
            (Lines{"rejected X id=9 request=cancel reason=unknown_order"}));
  EXPECT_EQ(h.cancel(test::cancel(0)),
            (Lines{"rejected X id=0 request=cancel reason=unknown_order"}));
  h.cancel(test::cancel(1));
  EXPECT_EQ(h.cancel(test::cancel(1)),
            (Lines{"rejected X id=1 request=cancel reason=unknown_order"}));
}

TYPED_TEST(BookTest, CancelOfAFilledOrderIsRejected) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(market(2, kBuy, 5));
  EXPECT_EQ(h.cancel(test::cancel(1)),
            (Lines{"rejected X id=1 request=cancel reason=unknown_order"}));
}

// --- Modify ------------------------------------------------------------------------

TYPED_TEST(BookTest, SmallerSizeKeepsTimePriority) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 10));
  h.submit(limit(2, kBuy, 100, 10));
  EXPECT_EQ(h.modify(test::modify(1, 100, 4)),
            (Lines{"modified X id=1 price=100 qty=4 priority=kept",
                   "delta X bid price=100 qty=14 orders=2"}));
  EXPECT_EQ(h.queue(kBuy, 100), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(h.submit(market(3, kSell, 4))[1],
            "fill X taker=3 maker=1 side=sell price=100 qty=4 taker_left=0 maker_left=0");
}

TYPED_TEST(BookTest, UnchangedModifyIsANoOpThatKeepsPriority) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 10));
  h.submit(limit(2, kBuy, 100, 10));
  EXPECT_EQ(h.modify(test::modify(1, 100, 10)),
            (Lines{"modified X id=1 price=100 qty=10 priority=kept"}));
  EXPECT_EQ(h.queue(kBuy, 100), (std::vector<std::uint64_t>{1, 2}));
}

TYPED_TEST(BookTest, LargerSizeLosesTimePriority) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 10));
  h.submit(limit(2, kBuy, 100, 10));
  EXPECT_EQ(h.modify(test::modify(1, 100, 15)),
            (Lines{"modified X id=1 price=100 qty=15 priority=lost",
                   "delta X bid price=100 qty=25 orders=2"}));
  EXPECT_EQ(h.queue(kBuy, 100), (std::vector<std::uint64_t>{2, 1}));
}

TYPED_TEST(BookTest, PriceChangeLosesTimePriority) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 10));
  h.submit(limit(2, kBuy, 99, 10));
  EXPECT_EQ(
      h.modify(test::modify(2, 100, 7)),
      (Lines{"modified X id=2 price=100 qty=7 priority=lost", "delta X bid price=99 qty=0 orders=0",
             "delta X bid price=100 qty=17 orders=2"}));
  EXPECT_EQ(h.queue(kBuy, 100), (std::vector<std::uint64_t>{1, 2}));
  // Moving back to the original price does not restore the old priority.
  h.submit(limit(3, kBuy, 99, 1));
  h.modify(test::modify(2, 99, 7));
  EXPECT_EQ(h.queue(kBuy, 99), (std::vector<std::uint64_t>{3, 2}));
}

TYPED_TEST(BookTest, ModifyToACrossingPriceMatches) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(limit(2, kBuy, 99, 8));
  EXPECT_EQ(
      h.modify(test::modify(2, 101, 8)),
      (Lines{"modified X id=2 price=101 qty=8 priority=lost", "delta X bid price=99 qty=0 orders=0",
             "fill X taker=2 maker=1 side=buy price=101 qty=5 taker_left=3 maker_left=0",
             "delta X ask price=101 qty=0 orders=0", "delta X bid price=101 qty=3 orders=1"}));
  EXPECT_EQ(h.book.find(OrderId{2})->qty, Qty{3});
}

TYPED_TEST(BookTest, ModifyThatFillsCompletelyRemovesTheOrder) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 9));
  h.submit(limit(2, kBuy, 99, 8));
  h.modify(test::modify(2, 102, 8));
  EXPECT_EQ(h.book.find(OrderId{2}), std::nullopt);
  EXPECT_EQ(h.book.best(kSell), (LevelView{Price{101}, Qty{1}, 1}));
}

TYPED_TEST(BookTest, PostOnlyModifyThatWouldCrossIsRejectedAndChangesNothing) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5));
  h.submit(test::order(2, kBuy, OrderType::post_only, 99, 5));
  EXPECT_EQ(h.modify(test::modify(2, 101, 5)),
            (Lines{"rejected X id=2 request=modify reason=post_only_would_cross"}));
  EXPECT_EQ(h.book.find(OrderId{2})->price, Price{99});
  EXPECT_EQ(h.modify(test::modify(2, 100, 5)),
            (Lines{"modified X id=2 price=100 qty=5 priority=lost",
                   "delta X bid price=99 qty=0 orders=0", "delta X bid price=100 qty=5 orders=1"}));
  EXPECT_EQ(h.book.find(OrderId{2})->type, OrderType::post_only);
}

TYPED_TEST(BookTest, ModifyValidatesBeforeLookingUpTheOrder) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 5));
  EXPECT_EQ(h.modify(test::modify(42, 100, 0)),
            (Lines{"rejected X id=42 request=modify reason=invalid_quantity"}));
  EXPECT_EQ(h.modify(test::modify(42, 100, 5)),
            (Lines{"rejected X id=42 request=modify reason=unknown_order"}));
  EXPECT_EQ(h.modify(test::modify(1, kMaxPrice.value() + 1, 5)),
            (Lines{"rejected X id=1 request=modify reason=invalid_price"}));
  EXPECT_EQ(h.modify(test::modify(1, 100, kMaxOrderQty.value() + 1)),
            (Lines{"rejected X id=1 request=modify reason=invalid_quantity"}));
  EXPECT_EQ(h.book.find(OrderId{1})->qty, Qty{5});
}

// --- Validation and limits ------------------------------------------------------

TYPED_TEST(BookTest, InvalidNewOrdersAreRejectedInADefinedOrder) {
  auto& h = this->h;
  EXPECT_EQ(h.submit(limit(0, kBuy, 100, 0)),
            (Lines{"rejected X id=0 request=new reason=invalid_order_id"}));
  EXPECT_EQ(h.submit(limit(1, kBuy, kMaxPrice.value() + 1, 0)),
            (Lines{"rejected X id=1 request=new reason=invalid_quantity"}));
  EXPECT_EQ(h.submit(limit(1, kBuy, 100, -5)),
            (Lines{"rejected X id=1 request=new reason=invalid_quantity"}));
  EXPECT_EQ(h.submit(limit(1, kBuy, 100, kMaxOrderQty.value() + 1)),
            (Lines{"rejected X id=1 request=new reason=invalid_quantity"}));
  EXPECT_EQ(h.submit(limit(1, kBuy, kMaxPrice.value() + 1, 5)),
            (Lines{"rejected X id=1 request=new reason=invalid_price"}));
  EXPECT_EQ(h.submit(limit(1, kSell, kMinPrice.value() - 1, 5)),
            (Lines{"rejected X id=1 request=new reason=invalid_price"}));
  EXPECT_EQ(h.book.order_count(), 0U);
}

TYPED_TEST(BookTest, DuplicateIdsAreRejectedWhileTheOrderIsLive) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 5));
  EXPECT_EQ(h.submit(limit(1, kSell, 200, 5)),
            (Lines{"rejected X id=1 request=new reason=duplicate_order_id"}));
  EXPECT_EQ(h.submit(market(1, kSell, 5)),
            (Lines{"rejected X id=1 request=new reason=duplicate_order_id"}));
  h.cancel(test::cancel(1));
  // Once the order is gone its id may be reused.
  EXPECT_EQ(h.submit(limit(1, kSell, 200, 5)).front(),
            "accepted X id=1 owner=0 side=sell type=limit price=200 qty=5");
}

TYPED_TEST(BookTest, FullBookRejectsOrdersThatCouldRest) {
  Harness<TypeParam> h(BookConfig{.max_orders = 2});
  h.submit(limit(1, kSell, 101, 5));
  h.submit(limit(2, kSell, 102, 5));
  EXPECT_EQ(h.submit(limit(3, kBuy, 101, 5)),
            (Lines{"rejected X id=3 request=new reason=book_full"}));
  EXPECT_EQ(h.submit(test::order(4, kBuy, OrderType::post_only, 90, 5)),
            (Lines{"rejected X id=4 request=new reason=book_full"}));
  // Orders that never rest are still accepted.
  EXPECT_EQ(h.submit(test::order(5, kBuy, OrderType::ioc, 101, 5)).size(), 3U);
  EXPECT_EQ(h.submit(limit(6, kBuy, 90, 5)).front(),
            "accepted X id=6 owner=0 side=buy type=limit price=90 qty=5");
  EXPECT_EQ(h.violation, std::nullopt);
}

// --- Self-trade prevention ----------------------------------------------------------

TYPED_TEST(BookTest, SelfTradesAreAllowedWithoutPrevention) {
  auto& h = this->h;
  h.submit(limit(1, kSell, 101, 5, 7));
  EXPECT_EQ(h.submit(limit(2, kBuy, 101, 5, 7))[1],
            "fill X taker=2 maker=1 side=buy price=101 qty=5 taker_left=0 maker_left=0");
}

TYPED_TEST(BookTest, CancelNewestCancelsTheIncomingRemainder) {
  Harness<TypeParam> h(BookConfig{.stp = StpPolicy::cancel_newest});
  h.submit(limit(1, kSell, 101, 5, 1));
  h.submit(limit(2, kSell, 101, 5, 7));
  h.submit(limit(3, kSell, 102, 5, 1));
  EXPECT_EQ(
      h.submit(limit(4, kBuy, 102, 12, 7)),
      (Lines{"accepted X id=4 owner=7 side=buy type=limit price=102 qty=12",
             "fill X taker=4 maker=1 side=buy price=101 qty=5 taker_left=7 maker_left=0",
             "delta X ask price=101 qty=5 orders=1", "cancelled X id=4 qty=7 reason=self_trade"}));
  EXPECT_EQ(h.depth(kSell),
            (std::vector<LevelView>{{Price{101}, Qty{5}, 1}, {Price{102}, Qty{5}, 1}}));
  EXPECT_EQ(h.book.best(kBuy), std::nullopt);
  // Nothing traded at a level where the own order is first: no level update.
  EXPECT_EQ(h.submit(limit(5, kBuy, 101, 1, 7)),
            (Lines{"accepted X id=5 owner=7 side=buy type=limit price=101 qty=1",
                   "cancelled X id=5 qty=1 reason=self_trade"}));
  EXPECT_EQ(h.violation, std::nullopt);
}

TYPED_TEST(BookTest, CancelOldestCancelsRestingOrdersAndKeepsMatching) {
  Harness<TypeParam> h(BookConfig{.stp = StpPolicy::cancel_oldest});
  h.submit(limit(1, kSell, 101, 5, 1));
  h.submit(limit(2, kSell, 101, 5, 7));
  h.submit(limit(3, kSell, 102, 5, 1));
  EXPECT_EQ(
      h.submit(limit(4, kBuy, 102, 12, 7)),
      (Lines{"accepted X id=4 owner=7 side=buy type=limit price=102 qty=12",
             "fill X taker=4 maker=1 side=buy price=101 qty=5 taker_left=7 maker_left=0",
             "cancelled X id=2 qty=5 reason=self_trade", "delta X ask price=101 qty=0 orders=0",
             "fill X taker=4 maker=3 side=buy price=102 qty=5 taker_left=2 maker_left=0",
             "delta X ask price=102 qty=0 orders=0", "delta X bid price=102 qty=2 orders=1"}));
  EXPECT_EQ(h.violation, std::nullopt);
}

TYPED_TEST(BookTest, AnonymousOrdersAreNeverSelfTrades) {
  Harness<TypeParam> h(BookConfig{.stp = StpPolicy::cancel_newest});
  h.submit(limit(1, kSell, 101, 5, 0));
  EXPECT_EQ(h.submit(limit(2, kBuy, 101, 5, 0))[1],
            "fill X taker=2 maker=1 side=buy price=101 qty=5 taker_left=0 maker_left=0");
}

TYPED_TEST(BookTest, FokCountsOnlyLiquidityAheadOfAnOwnOrderUnderCancelNewest) {
  Harness<TypeParam> h(BookConfig{.stp = StpPolicy::cancel_newest});
  h.submit(limit(1, kSell, 101, 5, 1));
  h.submit(limit(2, kSell, 101, 5, 7));
  h.submit(limit(3, kSell, 101, 5, 1));
  EXPECT_EQ(h.submit(test::order(4, kBuy, OrderType::fok, 101, 6, 7)),
            (Lines{"accepted X id=4 owner=7 side=buy type=fok price=101 qty=6",
                   "cancelled X id=4 qty=6 reason=fok_unfillable"}));
  EXPECT_EQ(h.submit(test::order(5, kBuy, OrderType::fok, 101, 5, 7)).size(), 3U);
  EXPECT_EQ(h.violation, std::nullopt);
}

TYPED_TEST(BookTest, FokSkipsOwnOrdersUnderCancelOldestAndStaysAtomic) {
  Harness<TypeParam> h(BookConfig{.stp = StpPolicy::cancel_oldest});
  h.submit(limit(1, kSell, 101, 5, 7));
  h.submit(limit(2, kSell, 101, 5, 1));
  // Not enough liquidity from other owners: killed, and the own order survives.
  EXPECT_EQ(h.submit(test::order(3, kBuy, OrderType::fok, 101, 6, 7)),
            (Lines{"accepted X id=3 owner=7 side=buy type=fok price=101 qty=6",
                   "cancelled X id=3 qty=6 reason=fok_unfillable"}));
  EXPECT_EQ(h.queue(kSell, 101), (std::vector<std::uint64_t>{1, 2}));
  // Enough: the own order is cancelled on the way.
  EXPECT_EQ(h.submit(test::order(4, kBuy, OrderType::fok, 101, 5, 7)),
            (Lines{"accepted X id=4 owner=7 side=buy type=fok price=101 qty=5",
                   "cancelled X id=1 qty=5 reason=self_trade",
                   "fill X taker=4 maker=2 side=buy price=101 qty=5 taker_left=0 maker_left=0",
                   "delta X ask price=101 qty=0 orders=0"}));
  EXPECT_EQ(h.violation, std::nullopt);
}

TYPED_TEST(BookTest, ModifyIntoAnOwnOrderAppliesSelfTradePrevention) {
  Harness<TypeParam> h(BookConfig{.stp = StpPolicy::cancel_newest});
  h.submit(limit(1, kSell, 101, 5, 7));
  h.submit(limit(2, kBuy, 99, 5, 7));
  EXPECT_EQ(
      h.modify(test::modify(2, 101, 5)),
      (Lines{"modified X id=2 price=101 qty=5 priority=lost", "delta X bid price=99 qty=0 orders=0",
             "cancelled X id=2 qty=5 reason=self_trade"}));
  EXPECT_EQ(h.book.find(OrderId{2}), std::nullopt);
  EXPECT_EQ(h.violation, std::nullopt);
}

TYPED_TEST(BookTest, ForEachOrderOnAMissingLevelVisitsNothing) {
  auto& h = this->h;
  h.submit(limit(1, kBuy, 100, 5));
  EXPECT_TRUE(h.queue(kBuy, 101).empty());
  EXPECT_TRUE(h.queue(kSell, 100).empty());
}

}  // namespace
}  // namespace ob
