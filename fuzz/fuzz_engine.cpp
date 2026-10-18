// SPDX-License-Identifier: MIT
// Differential libFuzzer target: parses the input as an order stream and
// replays it through the ladder (with a 64-tick window, so that the
// overflow and window shifts are exercised), the std::map baseline and the
// reference book. Aborts if their events ever differ, or if a book violates
// an invariant after any command. The first byte selects the self-trade
// prevention policy (byte % 3) and the id-map hash seed; the seed corpus in
// fuzz/corpus/engine/ starts every input with that byte on its own line.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <variant>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/types.hpp>

namespace {

[[noreturn]] void fail() {
  std::abort();
}

template <class EngineT>
void check(const EngineT& engine, ob::SymbolId symbol) {
  if (engine.find_book(symbol)->check_invariants()) {
    fail();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  ob::BookConfig config;
  config.stp = static_cast<ob::StpPolicy>(data[0] % 3);
  config.hash_seed = 1U + (data[0] / 3U);  // Reproducible, and varied across inputs.
  config.ladder_ticks = 64;
  config.initial_capacity = 4;
  ob::Engine ladder(config);
  ob::MapEngine map(config);
  ob::ReferenceEngine reference(config);
  ob::VectorSink expected;
  ob::VectorSink ladder_events;
  ob::VectorSink map_events;

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as text
  const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
  ob::CsvReader reader(text);
  while (const auto item = reader.next()) {
    if (!*item) {
      continue;
    }
    const ob::CsvRecord& record = **item;
    if (ladder.symbol_count() >= 8 && !ladder.find_symbol(record.symbol)) {
      continue;  // Keep the number of books small.
    }
    const ob::SymbolId symbol = ladder.add_symbol(record.symbol);
    static_cast<void>(map.add_symbol(record.symbol));
    static_cast<void>(reference.add_symbol(record.symbol));
    const ob::Command command = ob::to_command(record, symbol);

    expected.events.clear();
    ladder_events.events.clear();
    map_events.events.clear();
    reference.apply(command, expected);
    ladder.apply(command, ladder_events);
    map.apply(command, map_events);
    if (ladder_events.events != expected.events || map_events.events != expected.events) {
      fail();
    }
    check(ladder, symbol);
    check(map, symbol);
    check(reference, symbol);
  }
  return 0;
}
