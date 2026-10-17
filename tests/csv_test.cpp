// SPDX-License-Identifier: MIT
// CSV order-stream parsing and writing.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/types.hpp>

namespace ob {
namespace {

CsvRecord parse_ok(std::string_view line) {
  const auto result = parse_csv_record(line, 1);
  EXPECT_TRUE(result.has_value()) << line << ": "
                                  << (result.has_value() ? "" : to_string(result.error()));
  return result.has_value() ? *result : CsvRecord{};
}

CsvError parse_error(std::string_view line) {
  const auto result = parse_csv_record(line, 3);
  EXPECT_FALSE(result.has_value()) << line;
  return result.has_value() ? CsvError{} : result.error();
}

std::vector<std::string> read_all(std::string_view text) {
  std::vector<std::string> out;
  CsvReader reader(text);
  while (const auto item = reader.next()) {
    if (*item) {
      std::string line;
      append_csv(line, **item);
      out.push_back(line);
    } else {
      out.push_back("error " + to_string(item->error()));
    }
  }
  return out;
}

TEST(CsvTest, ParsesANewLimitOrder) {
  const CsvRecord record = parse_ok("new,AAPL,17,buy,limit,10050,100,7");
  EXPECT_EQ(record.action, CsvAction::new_order);
  EXPECT_EQ(record.symbol, "AAPL");
  EXPECT_EQ(record.id, OrderId{17});
  EXPECT_EQ(record.side, Side::buy);
  EXPECT_EQ(record.type, OrderType::limit);
  EXPECT_EQ(record.price, Price{10050});
  EXPECT_EQ(record.qty, Qty{100});
  EXPECT_EQ(record.owner, OwnerId{7});
}

TEST(CsvTest, ParsesEveryOrderType) {
  EXPECT_EQ(parse_ok("new,A,1,sell,market,,5").type, OrderType::market);
  EXPECT_EQ(parse_ok("new,A,1,sell,ioc,-3,5").type, OrderType::ioc);
  EXPECT_EQ(parse_ok("new,A,1,sell,fok,3,5").type, OrderType::fok);
  EXPECT_EQ(parse_ok("new,A,1,sell,post_only,3,5").type, OrderType::post_only);
  EXPECT_EQ(parse_ok("new,A,1,sell,ioc,-3,5").price, Price{-3});
}

TEST(CsvTest, OwnerIsOptional) {
  EXPECT_EQ(parse_ok("new,A,1,buy,limit,3,5").owner, OwnerId{0});
  EXPECT_EQ(parse_ok("new,A,1,buy,limit,3,5,").owner, OwnerId{0});
}

TEST(CsvTest, ParsesCancelAndModify) {
  const CsvRecord cancel = parse_ok("cancel,BTC/USD,99");
  EXPECT_EQ(cancel.action, CsvAction::cancel);
  EXPECT_EQ(cancel.symbol, "BTC/USD");
  EXPECT_EQ(cancel.id, OrderId{99});
  EXPECT_EQ(parse_ok("cancel,X,99,,,,,").action, CsvAction::cancel);

  const CsvRecord modify = parse_ok("modify,X,5,,,101,40");
  EXPECT_EQ(modify.action, CsvAction::modify);
  EXPECT_EQ(modify.price, Price{101});
  EXPECT_EQ(modify.qty, Qty{40});
}

TEST(CsvTest, TrimsBlanksAroundFields) {
  const CsvRecord record = parse_ok(" new , A ,\t1, buy ,limit , 3 ,5 ");
  EXPECT_EQ(record.symbol, "A");
  EXPECT_EQ(record.price, Price{3});
}

TEST(CsvTest, RangeChecksAreLeftToTheBook) {
  // Zero ids and quantities parse; the book rejects them with a reason.
  EXPECT_EQ(parse_ok("new,A,0,buy,limit,3,0").qty, Qty{0});
  EXPECT_EQ(parse_ok("new,A,1,buy,limit,3,-5").qty, Qty{-5});
  EXPECT_EQ(parse_ok("new,A,18446744073709551615,buy,limit,9223372036854775807,1").id,
            OrderId{18446744073709551615ULL});
}

TEST(CsvTest, ReportsTheFirstProblemWithItsColumn) {
  EXPECT_EQ(parse_error("buy,A,1"), (CsvError{3, 1, CsvErrorCode::unknown_action}));
  EXPECT_EQ(parse_error(""), (CsvError{3, 1, CsvErrorCode::unknown_action}));
  EXPECT_EQ(parse_error("new,,1,buy,limit,3,5"), (CsvError{3, 2, CsvErrorCode::missing_field}));
  EXPECT_EQ(parse_error("new,A B,1,buy,limit,3,5"), (CsvError{3, 2, CsvErrorCode::invalid_symbol}));
  EXPECT_EQ(parse_error("new,ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456,1,buy,limit,3,5"),
            (CsvError{3, 2, CsvErrorCode::invalid_symbol}));
  EXPECT_EQ(parse_error("new,A,,buy,limit,3,5"), (CsvError{3, 3, CsvErrorCode::missing_field}));
  EXPECT_EQ(parse_error("new,A,-1,buy,limit,3,5"), (CsvError{3, 3, CsvErrorCode::invalid_number}));
  EXPECT_EQ(parse_error("new,A,18446744073709551616,buy,limit,3,5"),
            (CsvError{3, 3, CsvErrorCode::out_of_range}));
  EXPECT_EQ(parse_error("new,A,1,bid,limit,3,5"), (CsvError{3, 4, CsvErrorCode::invalid_side}));
  EXPECT_EQ(parse_error("new,A,1,buy,stop,3,5"), (CsvError{3, 5, CsvErrorCode::invalid_type}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,,5"), (CsvError{3, 6, CsvErrorCode::missing_field}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,1.5,5"), (CsvError{3, 6, CsvErrorCode::invalid_number}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,+3,5"), (CsvError{3, 6, CsvErrorCode::invalid_number}));
  EXPECT_EQ(parse_error("new,A,1,buy,market,3,5"),
            (CsvError{3, 6, CsvErrorCode::unexpected_field}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,3"), (CsvError{3, 7, CsvErrorCode::missing_field}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,3,9223372036854775808"),
            (CsvError{3, 7, CsvErrorCode::out_of_range}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,3,5,4294967296"),
            (CsvError{3, 8, CsvErrorCode::out_of_range}));
  EXPECT_EQ(parse_error("new,A,1,buy,limit,3,5,7,8"),
            (CsvError{3, 0, CsvErrorCode::too_many_fields}));
  EXPECT_EQ(parse_error("cancel,A,1,buy"), (CsvError{3, 4, CsvErrorCode::unexpected_field}));
  EXPECT_EQ(parse_error("cancel,A,1,,,,,9"), (CsvError{3, 8, CsvErrorCode::unexpected_field}));
  EXPECT_EQ(parse_error("modify,A,1,buy,,3,5"), (CsvError{3, 4, CsvErrorCode::unexpected_field}));
  EXPECT_EQ(parse_error("modify,A,1,,,3"), (CsvError{3, 7, CsvErrorCode::missing_field}));
  EXPECT_EQ(parse_error("modify,A,1,,,3,5,1"), (CsvError{3, 8, CsvErrorCode::unexpected_field}));
}

TEST(CsvTest, ErrorsDescribeThemselves) {
  EXPECT_EQ(to_string(CsvError{3, 6, CsvErrorCode::invalid_number}),
            "line 3, field 6: invalid_number");
  EXPECT_EQ(to_string(CsvError{9, 0, CsvErrorCode::too_many_fields}), "line 9: too_many_fields");
  for (int code = 0; code <= static_cast<int>(CsvErrorCode::invalid_type); ++code) {
    EXPECT_NE(to_string(static_cast<CsvErrorCode>(code)), "?");
  }
}

TEST(CsvTest, ReaderSkipsCommentsBlanksAndOneHeader) {
  const std::string text =
      "# a comment\n"
      "\n"
      "action,symbol,id,side,type,price,qty,owner\r\n"
      "   # indented comment\n"
      "new,A,1,buy,limit,3,5,2\r\n"
      "   \t\n"
      "cancel,A,1";  // No final newline.
  EXPECT_EQ(read_all(text),
            (std::vector<std::string>{"new,A,1,buy,limit,3,5,2\n", "cancel,A,1\n"}));
}

TEST(CsvTest, ReaderAcceptsAHeaderOnlyBeforeTheFirstRecord) {
  const std::string text =
      "new,A,1,buy,limit,3,5\n"
      "action,symbol,id,side,type,price,qty,owner\n"
      "cancel,A,1\n";
  EXPECT_EQ(read_all(text),
            (std::vector<std::string>{"new,A,1,buy,limit,3,5,0\n",
                                      "error line 2, field 1: unknown_action", "cancel,A,1\n"}));
}

TEST(CsvTest, ReaderReportsLineNumbersAndContinues) {
  CsvReader reader("new,A,1,buy,limit,3,5\n\nnew,A,x,buy,limit,3,5\ncancel,A,1\n");
  ASSERT_TRUE(reader.next()->has_value());
  EXPECT_EQ(reader.line(), 1U);
  const auto bad = reader.next();
  ASSERT_FALSE(bad->has_value());
  EXPECT_EQ(bad->error(), (CsvError{3, 3, CsvErrorCode::invalid_number}));
  ASSERT_TRUE(reader.next()->has_value());
  EXPECT_EQ(reader.line(), 4U);
  EXPECT_EQ(reader.next(), std::nullopt);
}

TEST(CsvTest, WriterOutputParsesBackToTheSameRecord) {
  for (const std::string_view line :
       {"new,A,1,buy,limit,3,5,0", "new,SYM.1,2,sell,market,,5,9", "new,X,3,sell,fok,-7,1,0",
        "new,X,4,buy,post_only,9,9,4294967295", "cancel,B,18446744073709551615",
        "modify,C,7,,,-1,3"}) {
    const CsvRecord record = parse_ok(line);
    std::string written;
    append_csv(written, record);
    EXPECT_EQ(written, std::string(line) + "\n");
    EXPECT_EQ(parse_ok(std::string_view(written).substr(0, written.size() - 1)), record);
  }
  std::string header;
  append_csv_header(header);
  EXPECT_EQ(header, std::string(kCsvHeader) + "\n");
}

TEST(CsvTest, ConvertsBetweenRecordsAndCommands) {
  const CsvRecord record = parse_ok("new,A,1,sell,ioc,7,5,3");
  const Command command = to_command(record, SymbolId{4});
  const NewOrder expected{SymbolId{4},    OrderId{1}, OwnerId{3}, Side::sell,
                          OrderType::ioc, Price{7},   Qty{5}};
  EXPECT_EQ(std::get<NewOrder>(command), expected);
  EXPECT_EQ(to_csv_record(command, "A"), record);

  const Command cancel = to_command(parse_ok("cancel,A,9"), SymbolId{1});
  EXPECT_EQ(std::get<CancelOrder>(cancel), (CancelOrder{SymbolId{1}, OrderId{9}}));
  EXPECT_EQ(to_csv_record(cancel, "A"), parse_ok("cancel,A,9"));

  const Command modify = to_command(parse_ok("modify,A,9,,,5,6"), SymbolId{2});
  EXPECT_EQ(std::get<ModifyOrder>(modify),
            (ModifyOrder{SymbolId{2}, OrderId{9}, Price{5}, Qty{6}}));
  EXPECT_EQ(to_csv_record(modify, "A"), parse_ok("modify,A,9,,,5,6"));

  // A market order's price is not part of the record.
  NewOrder market = expected;
  market.type = OrderType::market;
  EXPECT_EQ(to_csv_record(market, "A").price, Price{0});
}

}  // namespace
}  // namespace ob
