// SPDX-License-Identifier: MIT
// Quick start: one engine, a few orders, every event printed as it happens.
//
//   cmake --preset dev && cmake --build --preset dev
//   ./build/dev/examples/example_basic

#include <cstdio>
#include <string>

#include <orderbook/orderbook.hpp>

int main() {
  ob::Engine engine(ob::BookConfig{.stp = ob::StpPolicy::cancel_newest});
  const ob::SymbolId aapl = engine.add_symbol("AAPL");

  // Any callable that accepts every event type is a sink. This one prints.
  auto print = [&](const auto& event) {
    std::string line;
    ob::append_event(line, event, engine.symbol_names());
    std::fputs(line.c_str(), stdout);
  };

  using ob::OrderId;
  using ob::OrderType;
  using ob::OwnerId;
  using ob::Price;
  using ob::Qty;
  using ob::Side;

  // Two asks at the same price: order 1 arrived first, so it trades first.
  engine.submit(
      {aapl, OrderId{1}, OwnerId{10}, Side::sell, OrderType::limit, Price{10050}, Qty{100}}, print);
  engine.submit(
      {aapl, OrderId{2}, OwnerId{11}, Side::sell, OrderType::limit, Price{10050}, Qty{100}}, print);
  // A bid below the asks rests.
  engine.submit(
      {aapl, OrderId{3}, OwnerId{12}, Side::buy, OrderType::limit, Price{10040}, Qty{300}}, print);
  // A marketable buy takes 150 lots: all of order 1, half of order 2.
  engine.submit({aapl, OrderId{4}, OwnerId{13}, Side::buy, OrderType::ioc, Price{10050}, Qty{150}},
                print);
  // Reducing order 3 keeps its place in the queue; repricing it would not.
  engine.modify({aapl, OrderId{3}, Price{10040}, Qty{200}}, print);
  engine.cancel({aapl, OrderId{2}}, print);

  const ob::OrderBook& book = *engine.find_book(aapl);
  if (const auto bid = book.best_bid()) {
    std::printf("best bid: %lld x %lld\n", static_cast<long long>(bid->price.value()),
                static_cast<long long>(bid->qty.value()));
  }
  std::printf("resting orders: %zu\n", book.order_count());
  return 0;
}
