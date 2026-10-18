// SPDX-License-Identifier: MIT
/// @file
/// Reading and writing order streams as CSV.
///
/// One request per line, eight columns:
///
///     action,symbol,id,side,type,price,qty,owner
///     new,AAPL,1,buy,limit,10050,100,7
///     new,AAPL,2,sell,market,,40,8
///     modify,AAPL,1,,,10050,60
///     cancel,AAPL,1
///
/// - `action` is `new`, `cancel` or `modify`.
/// - `symbol` is 1 to 32 characters from `A-Z a-z 0-9 . _ - /`.
/// - `id` is a decimal unsigned 64-bit integer.
/// - `side` is `buy` or `sell`; `type` is `limit`, `market`, `ioc`, `fok` or
///   `post_only` (new orders only).
/// - `price` is a signed decimal number of ticks (empty for market orders);
///   `qty` a signed decimal number of lots. Their ranges are checked by the
///   book, not the parser, so out-of-range values become `Rejected` events.
/// - `owner` is an optional unsigned 32-bit integer (new orders only).
///
/// Columns a request does not use must be empty and may be omitted at the end
/// of the line. Fields may be padded with spaces or tabs. Blank lines and
/// lines whose first non-blank character is `#` are ignored, and so is a
/// header line (first field `action`) before the first record. Lines may end
/// in `\n` or `\r\n`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <orderbook/commands.hpp>
#include <orderbook/expected.hpp>
#include <orderbook/types.hpp>

namespace ob {

/// The column names, in order, as written by `append_csv_header`.
inline constexpr std::string_view kCsvHeader = "action,symbol,id,side,type,price,qty,owner";

/// Maximum length of a symbol in a CSV file.
inline constexpr std::size_t kMaxSymbolLength = 32;

/// The request a CSV record describes.
enum class CsvAction : std::uint8_t {
  new_order,  ///< `new`
  cancel,     ///< `cancel`
  modify,     ///< `modify`
};

/// One parsed record. `symbol` refers to the text that was parsed.
struct CsvRecord {
  CsvAction action = CsvAction::new_order;  ///< Request kind.
  std::string_view symbol;                  ///< Symbol name.
  OrderId id{};                             ///< Order id.
  Side side = Side::buy;                    ///< Side (new orders).
  OrderType type = OrderType::limit;        ///< Type (new orders).
  Price price;                              ///< Price (new non-market orders, modify).
  Qty qty;                                  ///< Quantity (new orders, modify).
  OwnerId owner{};                          ///< Owner (new orders).

  /// Field-wise equality (compares the symbol text).
  friend bool operator==(const CsvRecord&, const CsvRecord&) = default;
};

/// What is wrong with a line.
enum class CsvErrorCode : std::uint8_t {
  unknown_action,    ///< First field is not `new`, `cancel` or `modify`.
  missing_field,     ///< A required field is empty or absent.
  too_many_fields,   ///< More than eight fields.
  unexpected_field,  ///< A field the request does not use is not empty.
  invalid_symbol,    ///< Symbol too long or with a character outside the allowed set.
  invalid_number,    ///< Not a decimal integer.
  out_of_range,      ///< A decimal integer that does not fit its type.
  invalid_side,      ///< Neither `buy` nor `sell`.
  invalid_type,      ///< Not an order type name.
};

/// Location and kind of a CSV error.
struct CsvError {
  std::size_t line = 0;                              ///< 1-based line number.
  std::size_t field = 0;                             ///< 1-based column, or 0 for the whole line.
  CsvErrorCode code = CsvErrorCode::unknown_action;  ///< What is wrong.

  /// Member-wise equality.
  friend bool operator==(const CsvError&, const CsvError&) = default;
};

/// Stable lower-case name of an error code.
[[nodiscard]] std::string_view to_string(CsvErrorCode code) noexcept;

/// "line 3, field 6: invalid_number".
[[nodiscard]] std::string to_string(const CsvError& error);

/// Parses one record. `line` must not contain a line terminator and must not
/// be blank or a comment (`CsvReader` skips those). `line_number` is only
/// used to fill in `CsvError::line`.
[[nodiscard]] expected<CsvRecord, CsvError> parse_csv_record(std::string_view line,
                                                             std::size_t line_number = 1) noexcept;

/// Reads the records of a CSV document one at a time, without copying it.
///
///     ob::CsvReader reader(text);
///     while (auto item = reader.next()) {
///       if (!*item) { report(item->error()); break; }
///       use(**item);
///     }
class CsvReader {
 public:
  /// Reads from `text`, which must outlive the reader and its records.
  explicit CsvReader(std::string_view text) noexcept : rest_(text) {}

  /// The next record or error; `std::nullopt` at the end of the input.
  /// After an error, reading continues with the following line.
  [[nodiscard]] std::optional<expected<CsvRecord, CsvError>> next() noexcept;

  /// Number of the line most recently read (1-based; 0 before the first).
  [[nodiscard]] std::size_t line() const noexcept { return line_; }

 private:
  std::string_view rest_;
  std::size_t line_ = 0;
  bool header_allowed_ = true;
};

/// Appends `kCsvHeader` and a newline.
void append_csv_header(std::string& out);

/// Appends `record` as one canonical line (no padding, unused trailing
/// columns omitted) followed by a newline. `parse_csv_record` reads it back
/// as an equal record.
void append_csv(std::string& out, const CsvRecord& record);

/// The command a record describes, addressed to `symbol`.
[[nodiscard]] Command to_command(const CsvRecord& record, SymbolId symbol) noexcept;

/// The record describing `command`, with symbol name `symbol`.
[[nodiscard]] CsvRecord to_csv_record(const Command& command, std::string_view symbol) noexcept;

}  // namespace ob
