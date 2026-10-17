// SPDX-License-Identifier: MIT
// Private helpers for building text output.
#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <string>

namespace ob::detail {

/// Appends the decimal representation of `value` (no locale, no allocation
/// beyond the string's own growth).
template <std::integral Int>
void append_number(std::string& out, Int value) {
  std::array<char, 24> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  out.append(buffer.data(), result.ptr);
}

}  // namespace ob::detail
