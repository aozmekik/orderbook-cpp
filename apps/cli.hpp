// SPDX-License-Identifier: MIT
// Small helpers shared by the command-line tools.
#pragma once

#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace ob::cli {

/// A `--name=value` (or `--name`) argument.
struct Option {
  std::string_view name;
  std::optional<std::string_view> value;
};

/// Splits `--name=value`; returns std::nullopt for arguments that are not options.
[[nodiscard]] inline std::optional<Option> parse_option(std::string_view arg) {
  if (arg.size() < 3 || !arg.starts_with("--")) {
    return std::nullopt;
  }
  arg.remove_prefix(2);
  const std::size_t equals = arg.find('=');
  if (equals == std::string_view::npos) {
    return Option{arg, std::nullopt};
  }
  return Option{arg.substr(0, equals), arg.substr(equals + 1)};
}

/// Parses a whole decimal integer.
template <class Int>
[[nodiscard]] std::optional<Int> parse_number(std::string_view text) {
  Int value{};
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [end, error] = std::from_chars(first, last, value);
  if (text.empty() || error != std::errc{} || end != last) {
    return std::nullopt;
  }
  return value;
}

/// Reads a whole file, or standard input for "-".
[[nodiscard]] inline std::optional<std::string> read_input(const std::string& path) {
  if (path == "-") {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return std::move(buffer).str();
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/// Writes `text` to `stream`. Errors are detected later with `finish`.
inline void write(std::FILE* stream, std::string_view text) {
  static_cast<void>(std::fwrite(text.data(), 1, text.size(), stream));
}

/// Writes `text` to standard output and clears it.
inline void flush_to_stdout(std::string& text) {
  write(stdout, text);
  text.clear();
}

/// Flushes standard output; returns `status`, or 1 if any write failed.
[[nodiscard]] inline int finish(int status) {
  if (std::fflush(stdout) != 0 || std::ferror(stdout) != 0) {
    write(stderr, "error: cannot write to standard output\n");
    return 1;
  }
  return status;
}

}  // namespace ob::cli
