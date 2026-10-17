// SPDX-License-Identifier: MIT
// Text rendering, and the multi-symbol engine.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/format.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/types.hpp>

#include "test_support.hpp"

namespace ob {
namespace {

using test::kBuy;
using test::kSell;

std::string format(const Event& event) {
  std::string out;
  const std::vector<std::string> names{"AAPL", "MSFT"};
  append_event(out, event, names);
  return out;
}

TEST(FormatTest, RendersEveryEventOnOneLine) {
  EXPECT_EQ(format(Accepted{SymbolId{0}, OrderId{1}, OwnerId{7}, kBuy, OrderType::limit,
                            Price{10050}, Qty{100}}),
            "accepted AAPL id=1 owner=7 side=buy type=limit price=10050 qty=100\n");
  EXPECT_EQ(format(Rejected{SymbolId{1}, OrderId{9}, Request::cancel, RejectReason::unknown_order}),
            "rejected MSFT id=9 request=cancel reason=unknown_order\n");
  EXPECT_EQ(format(Fill{SymbolId{0}, OrderId{2}, OrderId{1}, kSell, Price{10050}, Qty{40}, Qty{0},
                        Qty{60}}),
            "fill AAPL taker=2 maker=1 side=sell price=10050 qty=40 taker_left=0 maker_left=60\n");
  EXPECT_EQ(format(Cancelled{SymbolId{0}, OrderId{3}, Qty{10}, CancelReason::ioc_remainder}),
            "cancelled AAPL id=3 qty=10 reason=ioc_remainder\n");
  EXPECT_EQ(format(Modified{SymbolId{0}, OrderId{1}, Price{-5}, Qty{60}, false}),
            "modified AAPL id=1 price=-5 qty=60 priority=lost\n");
  EXPECT_EQ(format(Modified{SymbolId{0}, OrderId{1}, Price{5}, Qty{60}, true}),
            "modified AAPL id=1 price=5 qty=60 priority=kept\n");
  EXPECT_EQ(format(BookDelta{SymbolId{1}, kSell, Price{10060}, Qty{0}, 0}),
            "delta MSFT ask price=10060 qty=0 orders=0\n");
}

TEST(FormatTest, UnknownSymbolsPrintTheirId) {
  EXPECT_EQ(format(Cancelled{SymbolId{12}, OrderId{3}, Qty{1}, CancelReason::requested}),
            "cancelled #12 id=3 qty=1 reason=requested\n");
}

TEST(FormatTest, SnapshotIsALadderWithAsksOnTop) {
  OrderBook book;
  NullSink sink;
  book.submit(test::limit(1, kBuy, 99, 5), sink);
  book.submit(test::limit(2, kBuy, 98, 6), sink);
  book.submit(test::limit(3, kSell, 101, 7), sink);
  book.submit(test::limit(4, kSell, 102, 8), sink);
  book.submit(test::limit(5, kSell, 101, 1), sink);
  std::string out;
  append_snapshot(out, "AAPL", book, 10);
  EXPECT_EQ(out,
            "snapshot AAPL bids=2 asks=2\n"
            "level AAPL ask price=102 qty=8 orders=1\n"
            "level AAPL ask price=101 qty=8 orders=2\n"
            "level AAPL bid price=99 qty=5 orders=1\n"
            "level AAPL bid price=98 qty=6 orders=1\n");
  out.clear();
  append_snapshot(out, "AAPL", book, 1);
  EXPECT_EQ(out,
            "snapshot AAPL bids=1 asks=1\n"
            "level AAPL ask price=101 qty=8 orders=2\n"
            "level AAPL bid price=99 qty=5 orders=1\n");
}

TEST(EngineTest, RegistersSymbolsWithDenseIds) {
  Engine engine;
  EXPECT_EQ(engine.add_symbol("AAPL"), SymbolId{0});
  EXPECT_EQ(engine.add_symbol("MSFT"), SymbolId{1});
  EXPECT_EQ(engine.add_symbol("AAPL"), SymbolId{0});
  EXPECT_EQ(engine.symbol_count(), 2U);
  EXPECT_EQ(engine.find_symbol("MSFT"), SymbolId{1});
  EXPECT_EQ(engine.find_symbol("GOOG"), std::nullopt);
  ASSERT_EQ(engine.symbol_names().size(), 2U);
  EXPECT_EQ(engine.symbol_names()[1], "MSFT");
  ASSERT_NE(engine.find_book(SymbolId{1}), nullptr);
  EXPECT_EQ(engine.find_book(SymbolId{1})->config().symbol, SymbolId{1});
  EXPECT_EQ(engine.find_book(SymbolId{2}), nullptr);
}

TEST(EngineTest, RejectsRequestsForUnknownSymbols) {
  Engine engine;
  VectorSink sink;
  engine.submit(
      NewOrder{SymbolId{3}, OrderId{1}, OwnerId{0}, kBuy, OrderType::limit, Price{1}, Qty{1}},
      sink);
  engine.cancel(CancelOrder{SymbolId{3}, OrderId{1}}, sink);
  engine.modify(ModifyOrder{SymbolId{3}, OrderId{1}, Price{1}, Qty{1}}, sink);
  EXPECT_EQ(test::render(sink.events),
            (std::vector<std::string>{"rejected #3 id=1 request=new reason=unknown_symbol",
                                      "rejected #3 id=1 request=cancel reason=unknown_symbol",
                                      "rejected #3 id=1 request=modify reason=unknown_symbol"}));
}

TEST(EngineTest, BooksAreIndependent) {
  Engine engine(BookConfig{.stp = StpPolicy::cancel_newest});
  const SymbolId x = engine.add_symbol("X");
  const SymbolId y = engine.add_symbol("Y");
  VectorSink sink;
  // The same id on two symbols, at prices that would cross within one book.
  engine.apply(NewOrder{x, OrderId{1}, OwnerId{0}, kBuy, OrderType::limit, Price{100}, Qty{5}},
               sink);
  engine.apply(NewOrder{y, OrderId{1}, OwnerId{0}, kSell, OrderType::limit, Price{90}, Qty{5}},
               sink);
  engine.apply(ModifyOrder{y, OrderId{1}, Price{91}, Qty{5}}, sink);
  engine.apply(CancelOrder{x, OrderId{1}}, sink);
  EXPECT_EQ(test::render(sink.events),
            (std::vector<std::string>{
                "accepted X id=1 owner=0 side=buy type=limit price=100 qty=5",
                "delta X bid price=100 qty=5 orders=1",
                "accepted Y id=1 owner=0 side=sell type=limit price=90 qty=5",
                "delta Y ask price=90 qty=5 orders=1",
                "modified Y id=1 price=91 qty=5 priority=lost",
                "delta Y ask price=90 qty=0 orders=0",
                "delta Y ask price=91 qty=5 orders=1",
                "cancelled X id=1 qty=5 reason=requested",
                "delta X bid price=100 qty=0 orders=0",
            }));
  EXPECT_EQ(engine.find_book(x)->order_count(), 0U);
  EXPECT_EQ(engine.find_book(y)->order_count(), 1U);
  EXPECT_EQ(engine.find_book(y)->config().stp, StpPolicy::cancel_newest);
}

TEST(EngineTest, WorksWithEveryBookImplementation) {
  const auto run = [](auto engine) {
    const SymbolId s = engine.add_symbol("X");
    VectorSink sink;
    engine.apply(NewOrder{s, OrderId{1}, OwnerId{0}, kSell, OrderType::limit, Price{10}, Qty{3}},
                 sink);
    engine.apply(NewOrder{s, OrderId{2}, OwnerId{0}, kBuy, OrderType::market, Price{0}, Qty{5}},
                 sink);
    return test::render(sink.events);
  };
  const std::vector<std::string> expected = run(Engine{});
  EXPECT_EQ(run(MapEngine{}), expected);
  EXPECT_EQ(run(ReferenceEngine{}), expected);
  EXPECT_EQ(expected.size(), 6U);
}

}  // namespace
}  // namespace ob
