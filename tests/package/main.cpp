// SPDX-License-Identifier: MIT
// Smoke test for the installed or fetched package: one trade, printed, and
// the compiled CSV functions, whose return type is part of the library's ABI.

#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

#include <orderbook/orderbook.hpp>

namespace {

int check(bool ok, std::string_view what) {
  if (!ok) {
    std::cerr << "consumer: " << what << " failed\n";
  }
  return ok ? 0 : 1;
}

}  // namespace

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
  int failures = check(out == kExpected, "fill output");

  const auto bad = ob::parse_csv_record("bogus,XYZ,1", 3);
  failures += check(!bad.has_value() && bad.error().line == 3 &&
                        bad.error().code == ob::CsvErrorCode::unknown_action,
                    "parse_csv_record on a bad line");
  ob::CsvReader reader("new,XYZ,7,buy,limit,10050,100,7\n");
  const auto item = reader.next();
  failures += check(item.has_value() && item->has_value() && (**item).id == ob::OrderId{7} &&
                        (**item).price == ob::Price{10050} && !reader.next().has_value(),
                    "CsvReader::next on a valid line");
  std::cout << "OB_USE_STD_EXPECTED=" << OB_USE_STD_EXPECTED << " __cplusplus=" << __cplusplus
            << '\n';
  return failures == 0 ? 0 : 1;
}
