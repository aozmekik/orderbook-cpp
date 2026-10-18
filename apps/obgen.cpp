// SPDX-License-Identifier: MIT
// obgen: writes a synthetic order stream as CSV to standard output and a
// summary of what the stream does to standard error.

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
  --max-distance=N    passive orders go 0..N-1 ticks behind the touch [128]
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

int run(int argc, char** argv) {
  ob::workload::Config config;
  bool quiet = false;
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  for (const char* raw : args.subspan(1)) {
    const std::optional<ob::cli::Option> option = ob::cli::parse_option(raw);
    if (!option) {
      return usage_error("unexpected argument " + std::string(raw));
    }
    if (option->name == "help") {
      ob::cli::write(stdout, kUsage);
      return ob::cli::finish(0);
    }
    if (option->name == "quiet") {
      quiet = true;
      continue;
    }
    const std::string_view value = option->value.value_or("");
    const auto u64 = ob::cli::parse_number<std::uint64_t>(value);
    const auto u32 = ob::cli::parse_number<std::uint32_t>(value);
    const auto i64 = ob::cli::parse_number<std::int64_t>(value);
    const auto set = [&](auto& field, const auto& parsed) {
      if (parsed) {
        field = *parsed;
      }
      return parsed.has_value();
    };
    bool ok = false;
    const std::string_view name = option->name;
    if (name == "commands") {
      ok = set(config.commands, u64);
    } else if (name == "symbols") {
      ok = set(config.symbols, u32) && config.symbols > 0;
    } else if (name == "seed") {
      ok = set(config.seed, u64);
    } else if (name == "start-price") {
      ok = set(config.start_price, i64);
    } else if (name == "max-distance") {
      ok = set(config.max_distance, u32) && config.max_distance > 0;
    } else if (name == "target-orders") {
      ok = set(config.target_orders, u32) && config.target_orders > 0;
    } else if (name == "owners") {
      ok = set(config.owners, u32) && config.owners > 0;
    } else if (name == "passive") {
      ok = set(config.mix.passive, u32);
    } else if (name == "cancel") {
      ok = set(config.mix.cancel, u32);
    } else if (name == "modify") {
      ok = set(config.mix.modify, u32);
    } else if (name == "aggressive") {
      ok = set(config.mix.aggressive, u32);
    } else if (name == "post-only") {
      ok = set(config.post_only_percent, u32) && config.post_only_percent <= 100;
    } else if (name == "market") {
      ok = set(config.market_percent, u32) && config.market_percent <= 100;
    } else if (name == "fok") {
      ok = set(config.fok_percent, u32) && config.fok_percent <= 100;
    } else {
      return usage_error("unknown option --" + std::string(name));
    }
    if (!ok) {
      return usage_error("invalid value for --" + std::string(name));
    }
  }
  if (config.market_percent + config.fok_percent > 100) {
    return usage_error("--market plus --fok must not exceed 100");
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
