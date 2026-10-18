// SPDX-License-Identifier: MIT
// obgen: writes a synthetic order stream as CSV to standard output and a
// summary of what the stream does to standard error.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/types.hpp>
#include <orderbook/workload.hpp>

#include "cli.hpp"

namespace {

constexpr std::string_view kUsage = R"(usage: obgen [options] > orders.csv

Generates a synthetic order stream: passive orders placed near the touch with
a 1/(d+1) distance profile, marketable orders that consume it, and many
cancels. Every cancel and modify targets an order that is live at that point
(the generator replays the stream through the reference book, with
self-trade prevention off, while it generates). The data is synthetic.

options (defaults in brackets):
  --commands=N        number of commands [1000000]
  --symbols=N         number of symbols, named SYM1, SYM2, ... [1]
  --seed=N            random seed; equal seeds give equal output [1]
  --start-price=N     initial reference price in ticks [100000]
  --max-distance=N    passive orders go 0..N-1 ticks behind the touch,
                      1 <= N <= 1048576 [128]
  --target-orders=N   live orders per symbol at which the cancel weight is
                      exactly --cancel; it scales with the book [2000]
  --owners=N          number of distinct owners [32]
  --passive=W         weight of new passive orders [46]
  --cancel=W          weight of cancels [43]
  --modify=W          weight of modifies [3]
  --aggressive=W      weight of marketable orders [8]
  --post-only=PCT     share of passive orders that are post-only [10]
  --market=PCT        share of marketable orders that are market orders [30]
  --fok=PCT           share of marketable orders that are fill-or-kill [5]
  --quiet             do not print the summary
  --help              show this message
)";

int usage_error(std::string_view message) {
  ob::cli::write(stderr, std::format("obgen: {}\n(see obgen --help)\n", message));
  return 2;
}

/// "12.3%" of `part` in `whole`, computed with integers.
std::string percent(std::uint64_t part, std::uint64_t whole) {
  if (whole == 0) {
    return "0.0%";
  }
  const std::uint64_t tenths = ((part * 1000) + (whole / 2)) / whole;
  return std::format("{}.{}%", tenths / 10, tenths % 10);
}

void print_summary(const ob::workload::Workload& workload) {
  const ob::workload::Stats& s = workload.stats;
  const std::uint64_t total = workload.commands.size();
  const std::uint64_t ended = s.ended_cancelled + s.ended_filled;
  ob::cli::write(
      stderr,
      std::format("obgen: {} commands over {} symbol(s): new {}, cancel {}, modify {}\n"
                  "obgen: {} new orders, {} marketable; {} rested, {} fills\n"
                  "obgen: of {} resting orders that left the book, {} were cancelled and {} "
                  "filled; {} still resting\n",
                  total, workload.symbols.size(), percent(s.new_orders, total),
                  percent(s.cancels, total), percent(s.modifies, total), s.new_orders,
                  percent(s.aggressive, s.new_orders), s.rested, s.fills, ended,
                  percent(s.ended_cancelled, ended), percent(s.ended_filled, ended),
                  s.live_at_end));
}

/// Sets the generator setting named by `--name=value`; returns false if
/// `value` is not valid for it (or `name` is not a setting).
bool set_option(ob::workload::Config& config, std::string_view name, std::string_view value) {
  const auto u64 = ob::cli::parse_number<std::uint64_t>(value);
  const auto u32 = ob::cli::parse_number<std::uint32_t>(value);
  const auto i64 = ob::cli::parse_number<std::int64_t>(value);
  const auto set = [](auto& field, const auto& parsed) {
    if (parsed) {
      field = *parsed;
    }
    return parsed.has_value();
  };
  if (name == "commands") {
    return set(config.commands, u64);
  }
  if (name == "symbols") {
    return set(config.symbols, u32) && config.symbols > 0;
  }
  if (name == "seed") {
    return set(config.seed, u64);
  }
  if (name == "start-price") {
    return set(config.start_price, i64);
  }
  if (name == "max-distance") {
    return set(config.max_distance, u32) && config.max_distance > 0;
  }
  if (name == "target-orders") {
    return set(config.target_orders, u32) && config.target_orders > 0;
  }
  if (name == "owners") {
    return set(config.owners, u32) && config.owners > 0;
  }
  if (name == "passive") {
    return set(config.mix.passive, u32);
  }
  if (name == "cancel") {
    return set(config.mix.cancel, u32);
  }
  if (name == "modify") {
    return set(config.mix.modify, u32);
  }
  if (name == "aggressive") {
    return set(config.mix.aggressive, u32);
  }
  if (name == "post-only") {
    return set(config.post_only_percent, u32) && config.post_only_percent <= 100;
  }
  if (name == "market") {
    return set(config.market_percent, u32) && config.market_percent <= 100;
  }
  if (name == "fok") {
    return set(config.fok_percent, u32) && config.fok_percent <= 100;
  }
  return false;
}

int run(int argc, char** argv) {
  ob::workload::Config config;
  bool quiet = false;
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  for (const char* raw : args.subspan(1)) {
    const std::optional<ob::cli::Option> option = ob::cli::parse_option(raw);
    if (!option) {
      return usage_error("unexpected argument " + std::string(raw));
    }
    constexpr std::array<std::string_view, 2> kFlags{"help", "quiet"};
    constexpr std::array<std::string_view, 14> kValued{
        "commands", "symbols", "seed",   "start-price", "max-distance", "target-orders", "owners",
        "passive",  "cancel",  "modify", "aggressive",  "post-only",    "market",        "fok"};
    const bool is_flag = std::ranges::find(kFlags, option->name) != kFlags.end();
    if (!is_flag && std::ranges::find(kValued, option->name) == kValued.end()) {
      return usage_error("unknown option --" + std::string(option->name));
    }
    if (const std::optional<std::string> error = ob::cli::shape_error(*option, !is_flag)) {
      return usage_error(*error);
    }
    if (option->name == "help") {
      ob::cli::write(stdout, kUsage);
      return ob::cli::finish(0);
    }
    if (option->name == "quiet") {
      quiet = true;
      continue;
    }
    if (!set_option(config, option->name, option->value.value_or(""))) {
      return usage_error("invalid value for --" + std::string(option->name));
    }
  }
  if (const std::optional<std::string> error = ob::workload::config_error(config)) {
    return usage_error(*error);
  }

  const ob::workload::Workload workload = ob::workload::generate(config);
  std::string out;
  ob::append_csv_header(out);
  for (const ob::Command& command : workload.commands) {
    const auto symbol = static_cast<std::size_t>(ob::to_underlying(ob::symbol_of(command)));
    ob::append_csv(out, ob::to_csv_record(command, workload.symbols[symbol]));
    if (out.size() >= (std::size_t{1} << 20U)) {
      ob::cli::flush_to_stdout(out);
    }
  }
  ob::cli::flush_to_stdout(out);
  if (!quiet) {
    print_summary(workload);
  }
  return ob::cli::finish(0);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    ob::cli::write(stderr, "obgen: ");
    ob::cli::write(stderr, error.what());
    ob::cli::write(stderr, "\n");
    return 1;
  }
}
