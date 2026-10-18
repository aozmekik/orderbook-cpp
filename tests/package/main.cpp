// SPDX-License-Identifier: MIT
// Smoke test for the installed or fetched package: one trade, printed.

#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

#include <orderbook/orderbook.hpp>

int main() {
  ob::Engine engine;
  const ob::SymbolId symbol = engine.add_symbol("XYZ");
  std::string out;
  auto sink = [&](const auto& event) {
    if constexpr (std::is_same_v<std::decay_t<decltype(event)>, ob::Fill>) {
      ob::append_event(out, event, engine.symbol_names());
    }
  };
  engine.submit({symbol, ob::OrderId{1}, ob::OwnerId{1}, ob::Side::sell, ob::OrderType::limit,
                 ob::Price{100}, ob::Qty{10}},
                sink);
  engine.submit({symbol, ob::OrderId{2}, ob::OwnerId{2}, ob::Side::buy, ob::OrderType::market,
                 ob::Price{0}, ob::Qty{4}},
                sink);
  std::cout << out;
  constexpr std::string_view kExpected =
      "fill XYZ taker=2 maker=1 side=buy price=100 qty=4 taker_left=0 maker_left=6\n";
  return out == kExpected ? 0 : 1;
}
