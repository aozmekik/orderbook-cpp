// SPDX-License-Identifier: MIT
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/expected.hpp>
#include <orderbook/types.hpp>

#include "text.hpp"

namespace ob {
namespace {

constexpr std::size_t kColumns = 8;

// 1-based column numbers.
constexpr std::size_t kActionColumn = 1;
constexpr std::size_t kSymbolColumn = 2;
constexpr std::size_t kIdColumn = 3;
constexpr std::size_t kSideColumn = 4;
constexpr std::size_t kTypeColumn = 5;
constexpr std::size_t kPriceColumn = 6;
constexpr std::size_t kQtyColumn = 7;
constexpr std::size_t kOwnerColumn = 8;

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

/// The trimmed fields of one line.
class Fields {
 public:
  /// Splits `line` on commas; returns std::nullopt if it has too many fields.
  [[nodiscard]] static std::optional<Fields> split(std::string_view line) noexcept {
    Fields fields;
    for (std::size_t start = 0;;) {
      if (fields.count_ == kColumns) {
        return std::nullopt;
      }
      const std::size_t comma = line.find(',', start);
      const std::size_t length = comma == std::string_view::npos ? comma : comma - start;
      fields.values_[fields.count_++] = trim(line.substr(start, length));
      if (comma == std::string_view::npos) {
        return fields;
      }
      start = comma + 1;
    }
  }

  /// Field `column` (1-based); empty if the line is shorter.
  [[nodiscard]] std::string_view operator[](std::size_t column) const noexcept {
    return column <= count_ ? values_[column - 1] : std::string_view{};
  }

 private:
  std::array<std::string_view, kColumns> values_{};
  std::size_t count_ = 0;
};

template <class Int>
[[nodiscard]] expected<Int, CsvErrorCode> parse_integer(std::string_view text) noexcept {
  Int value{};
  const char* const last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, value);
  if (error == std::errc::result_out_of_range) {
    return make_unexpected(CsvErrorCode::out_of_range);
  }
  if (error != std::errc{} || end != last) {
    return make_unexpected(CsvErrorCode::invalid_number);
  }
  return value;
}

[[nodiscard]] constexpr bool is_symbol_char(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '_' || c == '-' || c == '/';
}

[[nodiscard]] std::optional<Side> parse_side(std::string_view text) noexcept {
  if (text == "buy") {
    return Side::buy;
  }
  if (text == "sell") {
    return Side::sell;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<OrderType> parse_type(std::string_view text) noexcept {
  for (const OrderType type : {OrderType::limit, OrderType::market, OrderType::ioc, OrderType::fok,
                               OrderType::post_only}) {
    if (text == to_string(type)) {
      return type;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CsvAction> parse_action(std::string_view text) noexcept {
  if (text == "new") {
    return CsvAction::new_order;
  }
  if (text == "cancel") {
    return CsvAction::cancel;
  }
  if (text == "modify") {
    return CsvAction::modify;
  }
  return std::nullopt;
}

/// Parses the columns of one record, remembering the first error.
class RecordParser {
 public:
  RecordParser(const Fields& fields, std::size_t line) noexcept : fields_(fields), line_(line) {}

  /// Field `column` (1-based); empty if the line is shorter.
  [[nodiscard]] std::string_view field(std::size_t column) const noexcept {
    return fields_[column];
  }

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] CsvError error() const noexcept { return error_.value_or(CsvError{}); }

  /// Fails unless `column` is empty.
  void require_empty(std::size_t column) noexcept {
    if (ok() && !fields_[column].empty()) {
      fail(column, CsvErrorCode::unexpected_field);
    }
  }

  /// The non-empty text of `column`, or empty after recording missing_field.
  [[nodiscard]] std::string_view require(std::size_t column) noexcept {
    const std::string_view text = fields_[column];
    if (ok() && text.empty()) {
      fail(column, CsvErrorCode::missing_field);
    }
    return text;
  }

  template <class Int>
  [[nodiscard]] Int integer(std::size_t column) noexcept {
    const std::string_view text = require(column);
    if (!ok()) {
      return Int{};
    }
    const expected<Int, CsvErrorCode> value = parse_integer<Int>(text);
    if (!value) {
      fail(column, value.error());
      return Int{};
    }
    return *value;
  }

  void fail(std::size_t column, CsvErrorCode code) noexcept {
    if (ok()) {
      error_ = CsvError{line_, column, code};
    }
  }

 private:
  Fields fields_;
  std::size_t line_;
  std::optional<CsvError> error_;
};

}  // namespace

std::string_view to_string(CsvErrorCode code) noexcept {
  switch (code) {
    case CsvErrorCode::unknown_action:
      return "unknown_action";
    case CsvErrorCode::missing_field:
      return "missing_field";
    case CsvErrorCode::too_many_fields:
      return "too_many_fields";
    case CsvErrorCode::unexpected_field:
      return "unexpected_field";
    case CsvErrorCode::invalid_symbol:
      return "invalid_symbol";
    case CsvErrorCode::invalid_number:
      return "invalid_number";
    case CsvErrorCode::out_of_range:
      return "out_of_range";
    case CsvErrorCode::invalid_side:
      return "invalid_side";
    case CsvErrorCode::invalid_type:
      return "invalid_type";
  }
  return "?";
}

std::string to_string(const CsvError& error) {
  std::string out = "line ";
  detail::append_number(out, error.line);
  if (error.field != 0) {
    out += ", field ";
    detail::append_number(out, error.field);
  }
  out += ": ";
  out += to_string(error.code);
  return out;
}

expected<CsvRecord, CsvError> parse_csv_record(std::string_view line,
                                               std::size_t line_number) noexcept {
  const std::optional<Fields> fields = Fields::split(line);
  if (!fields) {
    return make_unexpected(CsvError{line_number, 0, CsvErrorCode::too_many_fields});
  }
  RecordParser parser(*fields, line_number);
  CsvRecord record;

  const std::optional<CsvAction> action = parse_action(parser.field(kActionColumn));
  if (!action) {
    return make_unexpected(CsvError{line_number, kActionColumn, CsvErrorCode::unknown_action});
  }
  record.action = *action;

  record.symbol = parser.require(kSymbolColumn);
  if (parser.ok()) {
    bool valid = record.symbol.size() <= kMaxSymbolLength;
    for (const char c : record.symbol) {
      valid = valid && is_symbol_char(c);
    }
    if (!valid) {
      parser.fail(kSymbolColumn, CsvErrorCode::invalid_symbol);
    }
  }
  record.id = OrderId{parser.integer<std::uint64_t>(kIdColumn)};

  switch (record.action) {
    case CsvAction::new_order: {
      const std::string_view side_text = parser.require(kSideColumn);
      if (const std::optional<Side> side = parse_side(side_text); parser.ok() && side) {
        record.side = *side;
      } else {
        parser.fail(kSideColumn, CsvErrorCode::invalid_side);
      }
      const std::string_view type_text = parser.require(kTypeColumn);
      if (const std::optional<OrderType> type = parse_type(type_text); parser.ok() && type) {
        record.type = *type;
      } else {
        parser.fail(kTypeColumn, CsvErrorCode::invalid_type);
      }
      if (record.type == OrderType::market) {
        parser.require_empty(kPriceColumn);
      } else {
        record.price = Price{parser.integer<std::int64_t>(kPriceColumn)};
      }
      record.qty = Qty{parser.integer<std::int64_t>(kQtyColumn)};
      if (!parser.field(kOwnerColumn).empty()) {
        record.owner = OwnerId{parser.integer<std::uint32_t>(kOwnerColumn)};
      }
      break;
    }
    case CsvAction::cancel:
      for (std::size_t column = kSideColumn; column <= kOwnerColumn; ++column) {
        parser.require_empty(column);
      }
      break;
    case CsvAction::modify:
      parser.require_empty(kSideColumn);
      parser.require_empty(kTypeColumn);
      record.price = Price{parser.integer<std::int64_t>(kPriceColumn)};
      record.qty = Qty{parser.integer<std::int64_t>(kQtyColumn)};
      parser.require_empty(kOwnerColumn);
      break;
  }

  if (!parser.ok()) {
    return make_unexpected(parser.error());
  }
  return record;
}

std::optional<expected<CsvRecord, CsvError>> CsvReader::next() noexcept {
  while (!rest_.empty()) {
    const std::size_t newline = rest_.find('\n');
    std::string_view line = rest_.substr(0, newline);
    rest_ = newline == std::string_view::npos ? std::string_view{} : rest_.substr(newline + 1);
    ++line_;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const std::string_view content = trim(line);
    if (content.empty() || content.front() == '#') {
      continue;
    }
    if (header_allowed_) {
      header_allowed_ = false;
      if (trim(content.substr(0, content.find(','))) == "action") {
        continue;
      }
    }
    return parse_csv_record(line, line_);
  }
  return std::nullopt;
}

void append_csv_header(std::string& out) {
  out += kCsvHeader;
  out += '\n';
}

void append_csv(std::string& out, const CsvRecord& record) {
  switch (record.action) {
    case CsvAction::new_order:
      out += "new,";
      out += record.symbol;
      out += ',';
      detail::append_number(out, to_underlying(record.id));
      out += ',';
      out += to_string(record.side);
      out += ',';
      out += to_string(record.type);
      out += ',';
      if (record.type != OrderType::market) {
        detail::append_number(out, record.price.value());
      }
      out += ',';
      detail::append_number(out, record.qty.value());
      out += ',';
      detail::append_number(out, to_underlying(record.owner));
      break;
    case CsvAction::cancel:
      out += "cancel,";
      out += record.symbol;
      out += ',';
      detail::append_number(out, to_underlying(record.id));
      break;
    case CsvAction::modify:
      out += "modify,";
      out += record.symbol;
      out += ',';
      detail::append_number(out, to_underlying(record.id));
      out += ",,,";
      detail::append_number(out, record.price.value());
      out += ',';
      detail::append_number(out, record.qty.value());
      break;
  }
  out += '\n';
}

Command to_command(const CsvRecord& record, SymbolId symbol) noexcept {
  switch (record.action) {
    case CsvAction::cancel:
      return CancelOrder{symbol, record.id};
    case CsvAction::modify:
      return ModifyOrder{symbol, record.id, record.price, record.qty};
    case CsvAction::new_order:
      break;
  }
  return NewOrder{symbol,      record.id,    record.owner, record.side,
                  record.type, record.price, record.qty};
}

CsvRecord to_csv_record(const Command& command, std::string_view symbol) noexcept {
  CsvRecord record;
  record.symbol = symbol;
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    record.action = CsvAction::new_order;
    record.id = order->id;
    record.side = order->side;
    record.type = order->type;
    record.price = order->type == OrderType::market ? Price{0} : order->price;
    record.qty = order->qty;
    record.owner = order->owner;
  } else if (const auto* cancel = std::get_if<CancelOrder>(&command)) {
    record.action = CsvAction::cancel;
    record.id = cancel->id;
  } else if (const auto* modify = std::get_if<ModifyOrder>(&command)) {
    record.action = CsvAction::modify;
    record.id = modify->id;
    record.price = modify->price;
    record.qty = modify->qty;
  }
  return record;
}

}  // namespace ob
