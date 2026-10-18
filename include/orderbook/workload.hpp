// SPDX-License-Identifier: MIT
/// @file
/// Synthetic order-flow generator for benchmarks, tests and `obgen`.
///
/// The model is a simplified zero-intelligence order flow in the spirit of
/// Cont, Stoikov and Talreja (2010) and Bouchaud, Mezard and Potters (2002):
/// passive orders arrive at a distance from the touch that decays like a
/// power law, marketable orders consume the touch, and most orders are
/// cancelled rather than filled. The generator replays the stream through a
/// `ReferenceBook` as it generates (with self-trade prevention off), so every
/// cancel and modify targets an order that is live at that point of the
/// stream, and passive prices follow the book as it moves. The output is
/// synthetic data, not a recording of a market.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <orderbook/commands.hpp>

namespace ob::workload {

/// Relative weights of the request kinds (they need not sum to 100).
struct Mix {
  std::uint32_t passive = 46;  ///< New limit orders behind or at the touch.
  /// Cancels of live orders. Every live order is equally likely to be
  /// cancelled, so this weight scales with the book: it applies as given
  /// when a symbol has `Config::target_orders` live orders, at half that it
  /// counts half, and so on. The book size settles near the target.
  std::uint32_t cancel = 43;
  std::uint32_t modify = 3;      ///< Size or price changes of live orders.
  std::uint32_t aggressive = 8;  ///< Marketable orders: market, IOC or FOK.
};

/// Generator settings. The defaults produce a cancel-heavy, near-touch flow.
struct Config {
  std::uint64_t seed = 1;              ///< Equal seeds give equal workloads everywhere.
  std::uint64_t commands = 1'000'000;  ///< Number of commands to generate.
  std::uint32_t symbols = 1;           ///< Number of symbols (named SYM1, SYM2, ...).
  /// Initial reference price, in ticks. At most `max_start_price(max_distance)`
  /// in absolute value, so that the first orders have valid prices and the
  /// generator's arithmetic cannot overflow. (Prices follow the book, so a
  /// stream that starts at the edge can drift past it; books reject those
  /// orders with `invalid_price`.)
  std::int64_t start_price = 100'000;
  /// Passive orders are placed `d` ticks behind the touch, `0 <= d <
  /// max_distance`, with probability proportional to `1 / (d + 1)`. At least
  /// 1 and at most `kMaxDistance`.
  std::uint32_t max_distance = 128;
  /// Live orders per symbol at which the cancel weight equals `mix.cancel`.
  std::uint32_t target_orders = 2'000;
  std::uint32_t owners = 32;             ///< Owners are drawn uniformly from 1..owners.
  std::uint32_t post_only_percent = 10;  ///< Share of passive orders that are post-only.
  std::uint32_t market_percent = 30;     ///< Share of aggressive orders that are market.
  std::uint32_t fok_percent = 5;         ///< Share of aggressive orders that are FOK.
  Mix mix{};                             ///< Request mix.
};

/// What the generated stream did when the generator replayed it.
struct Stats {
  std::uint64_t new_orders = 0;       ///< New-order commands.
  std::uint64_t cancels = 0;          ///< Cancel commands.
  std::uint64_t modifies = 0;         ///< Modify commands.
  std::uint64_t aggressive = 0;       ///< New orders that were marketable when sent.
  std::uint64_t fills = 0;            ///< Fill events.
  std::uint64_t rested = 0;           ///< Orders that rested in the book.
  std::uint64_t ended_cancelled = 0;  ///< Resting orders that ended by a cancel.
  std::uint64_t ended_filled = 0;     ///< Resting orders that ended fully filled.
  std::uint64_t live_at_end = 0;      ///< Orders still resting after the last command.
};

/// A generated stream.
struct Workload {
  std::vector<std::string> symbols;  ///< Symbol names, indexed by `SymbolId`.
  std::vector<Command> commands;     ///< The commands, in order.
  Stats stats;                       ///< Replay statistics.
};

/// Largest `Config::max_distance` (the distance table holds one entry per tick).
inline constexpr std::uint32_t kMaxDistance = std::uint32_t{1} << 20U;

/// Largest absolute `Config::start_price` for a given `max_distance`: the
/// generator places orders up to `max_distance` plus a few ticks away from
/// the reference price, and the first of them must be valid prices.
[[nodiscard]] std::int64_t max_start_price(std::uint32_t max_distance) noexcept;

/// Describes the first setting of `config` that `generate` does not accept,
/// or returns `std::nullopt` if there is none. Zero `symbols`, `owners` or
/// `target_orders` are accepted and treated as 1.
[[nodiscard]] std::optional<std::string> config_error(const Config& config);

/// Generates a workload. Deterministic: the result depends only on `config`.
/// Throws `std::invalid_argument` (with the `config_error` text) if `config`
/// is not accepted.
[[nodiscard]] Workload generate(const Config& config);

}  // namespace ob::workload
