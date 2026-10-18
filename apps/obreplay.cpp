// SPDX-License-Identifier: MIT
// obreplay: replays a CSV order stream through a matching engine and prints
// the trades and the final book. Standard output is deterministic; timing
// (with --stats) goes to standard error.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/csv.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/expected.hpp>
#include <orderbook/format.hpp>
#include <orderbook/types.hpp>

#include "cli.hpp"

namespace {

constexpr std::string_view kUsage = R"(usage: obreplay [options] <orders.csv | ->

Replays an order stream (CSV, see docs/csv-format.md) and prints every trade
followed by a snapshot of each book.

options:
  --events            print every event (accepted, rejected, fill, cancelled,
                      modified, delta) instead of only fills
  --engine=NAME       ladder (default), map or reference
  --stp=POLICY        self-trade prevention: none (default), cancel_newest,
                      cancel_oldest
  --depth=N           levels per side in the final snapshots (default 10)
  --ladder-ticks=N    ladder window in ticks (default 4096)
  --stats             print a timing summary to standard error
  --help              show this message

exit status: 0 on success, 1 if the input cannot be read or parsed, 2 on
usage errors.
)";

enum class EngineKind : std::uint8_t { ladder, map, reference };

struct Options {
  std::string path;
  EngineKind engine = EngineKind::ladder;
  ob::StpPolicy stp = ob::StpPolicy::none;
  std::size_t depth = 10;
  std::uint32_t ladder_ticks = 4096;
  bool all_events = false;
  bool stats = false;
};

int usage_error(std::string_view message) {
  ob::cli::write(stderr, std::format("obreplay: {}\n(see obreplay --help)\n", message));
  return 2;
}

std::optional<ob::StpPolicy> parse_stp(std::string_view text) {
  for (const ob::StpPolicy policy :
       {ob::StpPolicy::none, ob::StpPolicy::cancel_newest, ob::StpPolicy::cancel_oldest}) {
    if (text == ob::to_string(policy)) {
      return policy;
    }
  }
  return std::nullopt;
}

/// Formats events into a buffer that is flushed to stdout as it grows.
class PrintingSink {
 public:
  PrintingSink(std::span<const std::string> symbols, bool all_events)
      : symbols_(symbols), all_events_(all_events) {}

  template <class E>
  void operator()(const E& event) {
    ++events_;
    if constexpr (std::is_same_v<E, ob::Fill>) {
      ++fills_;
    } else if (!all_events_) {
      return;
    }
    ob::append_event(out_, event, symbols_);
    if (out_.size() >= kFlushBytes) {
      ob::cli::flush_to_stdout(out_);
    }
  }

  std::string& buffer() noexcept { return out_; }
  [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }

 private:
  static constexpr std::size_t kFlushBytes = std::size_t{1} << 20U;
  std::span<const std::string> symbols_;
  bool all_events_;
  std::string out_;
  std::uint64_t events_ = 0;
  std::uint64_t fills_ = 0;
};

template <class EngineT>
int replay(const Options& options, const std::string& text) {
  ob::BookConfig config;
  config.stp = options.stp;
  config.ladder_ticks = options.ladder_ticks;
  EngineT engine(config);

  // Parse everything first so that a bad line fails before any output.
  std::vector<ob::Command> commands;
  ob::CsvReader reader(text);
  while (const auto item = reader.next()) {
    if (!*item) {
      ob::cli::write(stderr,
                     std::format("obreplay: {}: {}\n", options.path, ob::to_string(item->error())));
      return 1;
    }
    const ob::CsvRecord& record = **item;
    commands.push_back(ob::to_command(record, engine.add_symbol(record.symbol)));
  }

  PrintingSink sink(engine.symbol_names(), options.all_events);
  const auto start = std::chrono::steady_clock::now();
  for (const ob::Command& command : commands) {
    engine.apply(command, sink);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;

  std::string& out = sink.buffer();
  for (std::size_t i = 0; i < engine.symbol_count(); ++i) {
    const auto symbol = static_cast<ob::SymbolId>(i);
    ob::append_snapshot(out, engine.symbol_names()[i], *engine.find_book(symbol), options.depth);
  }
  ob::cli::flush_to_stdout(out);

  if (options.stats) {
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const std::size_t count = commands.size();
    const double per_command =
        count == 0 ? 0.0 : static_cast<double>(nanos) / static_cast<double>(count);
    ob::cli::write(stderr, std::format("obreplay: {} commands, {} events, {} fills, {} symbols in "
                                       "{:.3f} ms ({:.0f} ns/command, includes formatting)\n",
                                       count, sink.events(), sink.fills(), engine.symbol_count(),
                                       static_cast<double>(nanos) / 1e6, per_command));
  }
  return ob::cli::finish(0);
}

int run(int argc, char** argv) {
  Options options;
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  for (const char* raw : args.subspan(1)) {
    const std::string_view arg(raw);
    const std::optional<ob::cli::Option> option = ob::cli::parse_option(arg);
    if (!option) {
      if (!options.path.empty()) {
        return usage_error("more than one input file");
      }
      options.path = arg;
      continue;
    }
    constexpr std::array<std::string_view, 3> kFlags{"help", "events", "stats"};
    constexpr std::array<std::string_view, 4> kValued{"engine", "stp", "depth", "ladder-ticks"};
    const bool is_flag = std::ranges::find(kFlags, option->name) != kFlags.end();
    if (!is_flag && std::ranges::find(kValued, option->name) == kValued.end()) {
      return usage_error("unknown option --" + std::string(option->name));
    }
    if (const std::optional<std::string> error = ob::cli::shape_error(*option, !is_flag)) {
      return usage_error(*error);
    }
    const std::string_view value = option->value.value_or("");
    if (option->name == "help") {
      ob::cli::write(stdout, kUsage);
      return ob::cli::finish(0);
    }
    if (option->name == "events") {
      options.all_events = true;
    } else if (option->name == "stats") {
      options.stats = true;
    } else if (option->name == "engine") {
      if (value == "ladder") {
        options.engine = EngineKind::ladder;
      } else if (value == "map") {
        options.engine = EngineKind::map;
      } else if (value == "reference") {
        options.engine = EngineKind::reference;
      } else {
        return usage_error("--engine must be ladder, map or reference");
      }
    } else if (option->name == "stp") {
      const std::optional<ob::StpPolicy> policy = parse_stp(value);
      if (!policy) {
        return usage_error("--stp must be none, cancel_newest or cancel_oldest");
      }
      options.stp = *policy;
    } else if (option->name == "depth") {
      const std::optional<std::size_t> depth = ob::cli::parse_number<std::size_t>(value);
      if (!depth) {
        return usage_error("--depth needs a non-negative integer");
      }
      options.depth = *depth;
    } else if (option->name == "ladder-ticks") {
      const std::optional<std::uint32_t> ticks = ob::cli::parse_number<std::uint32_t>(value);
      if (!ticks || *ticks == 0) {
        return usage_error("--ladder-ticks needs a positive integer");
      }
      options.ladder_ticks = *ticks;
    } else {
      return usage_error("unknown option --" + std::string(option->name));
    }
  }
  if (options.path.empty()) {
    return usage_error("missing input file");
  }

  const ob::expected<std::string, std::string> text = ob::cli::read_input(options.path);
  if (!text) {
    ob::cli::write(stderr,
                   std::format("obreplay: cannot read {}: {}\n", options.path, text.error()));
    return 1;
  }
  switch (options.engine) {
    case EngineKind::ladder:
      return replay<ob::Engine>(options, *text);
    case EngineKind::map:
      return replay<ob::MapEngine>(options, *text);
    case EngineKind::reference:
      return replay<ob::ReferenceEngine>(options, *text);
  }
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    ob::cli::write(stderr, "obreplay: ");
    ob::cli::write(stderr, error.what());
    ob::cli::write(stderr, "\n");
    return 1;
  }
}
