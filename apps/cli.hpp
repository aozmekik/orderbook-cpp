// SPDX-License-Identifier: MIT
// Small helpers shared by the command-line tools.
#pragma once

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <orderbook/expected.hpp>

namespace ob::cli {

/// A `--name=value` (or `--name`) argument.
struct Option {
  std::string_view name;
  std::optional<std::string_view> value;
};

/// Why `option` is malformed for a flag (`takes_value` false, `--name`) or
/// a valued option (`--name=value`), or std::nullopt if it is well formed.
[[nodiscard]] inline std::optional<std::string> shape_error(const Option& option,
                                                            bool takes_value) {
  const std::string name(option.name);
  if (takes_value && !option.value) {
    return "--" + name + " needs a value (--" + name + "=...)";
  }
  if (!takes_value && option.value) {
    return "--" + name + " takes no value";
  }
  return std::nullopt;
}

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

/// Closes a `FILE*` owned by a `std::unique_ptr`. A function-pointer deleter
/// (`decltype(&std::fclose)`) drops glibc's attributes on `fclose`, which GCC
/// reports under -Wignored-attributes.
struct FileCloser {
  void operator()(std::FILE* file) const noexcept {
    static_cast<void>(std::fclose(file));  // NOLINT(cppcoreguidelines-owning-memory)
  }
};

/// Reads a whole file, or standard input for "-". Pipes and other
/// non-regular files work; a directory, a file that cannot be opened and a
/// read error are reported with a short reason.
[[nodiscard]] inline ob::expected<std::string, std::string> read_input(const std::string& path) {
  std::unique_ptr<std::FILE, FileCloser> owned;
  std::FILE* file = stdin;
  if (path != "-") {
    std::error_code status_error;
    if (std::filesystem::is_directory(path, status_error)) {
      return ob::make_unexpected(std::string("is a directory"));
    }
    owned.reset(std::fopen(path.c_str(), "rb"));  // NOLINT(cppcoreguidelines-owning-memory)
    if (!owned) {
      return ob::make_unexpected(std::generic_category().message(errno));
    }
    file = owned.get();
  }
  std::string text;
  std::array<char, std::size_t{1} << 16U> buffer{};
  for (std::size_t got = 0; (got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0;) {
    text.append(buffer.data(), got);
  }
  if (std::ferror(file) != 0) {
    return ob::make_unexpected(std::string("read error"));
  }
  return text;
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
