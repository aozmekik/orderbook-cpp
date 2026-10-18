// SPDX-License-Identifier: MIT
// libFuzzer target for the CSV order-stream parser.
//
// Properties checked on arbitrary bytes:
// - the reader terminates and never crashes (ASan/UBSan catch memory errors);
// - every error names a line that exists and a column in 0..8;
// - every record survives a round trip: writing it in canonical form and
//   parsing that again gives an equal record, and so does converting it to a
//   command and back.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <orderbook/csv.hpp>
#include <orderbook/types.hpp>

namespace {

[[noreturn]] void fail() {
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as text
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  std::size_t lines = 1;
  for (const char c : text) {
    lines += c == '\n' ? 1 : 0;
  }
  ob::CsvReader reader(text);
  while (const auto item = reader.next()) {
    if (!*item) {
      const ob::CsvError& error = item->error();
      if (error.line == 0 || error.line > lines || error.field > 8 ||
          ob::to_string(error).empty()) {
        fail();
      }
      continue;
    }
    const ob::CsvRecord& record = **item;
    std::string line;
    ob::append_csv(line, record);
    if (line.empty() || line.back() != '\n') {
      fail();
    }
    line.pop_back();
    const auto again = ob::parse_csv_record(line);
    if (!again || !(*again == record)) {
      fail();
    }
    if (!(ob::to_csv_record(ob::to_command(record, ob::SymbolId{0}), record.symbol) == record)) {
      fail();
    }
  }
  return 0;
}
