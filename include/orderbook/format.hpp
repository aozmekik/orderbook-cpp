// SPDX-License-Identifier: MIT
/// @file
/// Deterministic text rendering of events and book snapshots.
///
/// The output depends only on its input: no clocks, no locale, no addresses.
/// Replaying the same order stream therefore produces byte-identical text,
/// whichever book implementation produced the events.
///
///     accepted AAPL id=1 owner=7 side=buy type=limit price=10050 qty=100
///     rejected AAPL id=9 request=cancel reason=unknown_order
///     fill AAPL taker=2 maker=1 side=sell price=10050 qty=40 taker_left=0 maker_left=60
///     cancelled AAPL id=3 qty=10 reason=ioc_remainder
///     modified AAPL id=1 price=10040 qty=60 priority=lost
///     delta AAPL bid price=10050 qty=60 orders=1
///
/// A snapshot lists asks from the worst shown level down to the best, then
/// bids from the best down, like a price ladder:
///
///     snapshot AAPL bids=1 asks=1
///     level AAPL ask price=10060 qty=100 orders=2
///     level AAPL bid price=10050 qty=60 orders=1

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orderbook/book_api.hpp>
#include <orderbook/events.hpp>
#include <orderbook/types.hpp>

namespace ob {

/// Appends one line describing `event`. Symbols are printed by name using
/// `symbols[id]`, or as `#id` when `symbols` has no such entry.
void append_event(std::string& out, const Event& event, std::span<const std::string> symbols);

/// Appends a snapshot of the given levels (each best first) of `symbol`.
void append_snapshot(std::string& out, std::string_view symbol, std::span<const LevelView> bids,
                     std::span<const LevelView> asks);

/// Appends a snapshot of up to `max_levels` levels per side of `book`.
template <OrderBookLike Book>
void append_snapshot(std::string& out, std::string_view symbol, const Book& book,
                     std::size_t max_levels) {
  std::vector<LevelView> bids(max_levels);
  std::vector<LevelView> asks(max_levels);
  bids.resize(book.depth(Side::buy, bids));
  asks.resize(book.depth(Side::sell, asks));
  append_snapshot(out, symbol, bids, asks);
}

}  // namespace ob
