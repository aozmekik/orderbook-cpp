// SPDX-License-Identifier: MIT
/// @file
/// `ob::expected<T, E>`: the result type of the functions that can fail
/// without throwing (the CSV parser).
///
/// It is the library's own small fallback by default, and `std::expected`
/// when the library is configured with `OB_USE_STD_EXPECTED=ON` (C++23). The
/// choice is recorded in the generated `<orderbook/config.hpp>` when the
/// library is built, never made by a feature test in the including
/// translation unit: compiled functions return this type by value, and
/// return types are not part of a mangled name, so a C++23 program would
/// otherwise link against a C++20-built library and read one type as the
/// other. Code that sticks to `has_value()`, `operator bool`, `operator*`,
/// `operator->` and `error()` compiles unchanged against either.

#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include <orderbook/config.hpp>

#if OB_USE_STD_EXPECTED
#include <version>
#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
// The library was built with ob::expected = std::expected; so must this file be.
#error "orderbook was built with OB_USE_STD_EXPECTED=ON: compile this file as C++23"
#endif
#include <expected>
#endif

namespace ob {

namespace detail {

/// Fallback for `std::unexpected`: wraps an error value.
template <class E>
class UnexpectedFallback {
 public:
  /// Wraps `error`.
  constexpr explicit UnexpectedFallback(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
      : error_(std::move(error)) {}

  /// The wrapped error.
  [[nodiscard]] constexpr const E& error() const& noexcept { return error_; }
  /// The wrapped error.
  [[nodiscard]] constexpr E& error() & noexcept { return error_; }
  /// The wrapped error, moved out.
  [[nodiscard]] constexpr E&& error() && noexcept { return std::move(error_); }

 private:
  E error_;
};

/// True for specialisations of `UnexpectedFallback`.
template <class T>
inline constexpr bool kIsUnexpectedFallback = false;
/// True for specialisations of `UnexpectedFallback`.
template <class E>
inline constexpr bool kIsUnexpectedFallback<UnexpectedFallback<E>> = true;

/// Fallback for `std::expected`: holds either a value or an error.
///
/// Accessing the value of an object that holds an error (or vice versa) is a
/// precondition violation, exactly as with `operator*` on `std::expected`.
template <class T, class E>
class ExpectedFallback {
 public:
  /// The value type.
  using value_type = T;
  /// The error type.
  using error_type = E;

  /// Holds `value`.
  template <class U = T>
    requires std::is_constructible_v<T, U&&> &&
             (!std::is_same_v<std::remove_cvref_t<U>, ExpectedFallback>) &&
             (!kIsUnexpectedFallback<std::remove_cvref_t<U>>)
  // NOLINTNEXTLINE(*-explicit-constructor): implicit, as in std
  constexpr ExpectedFallback(U&& value)
      : storage_(std::in_place_index<0>, std::forward<U>(value)) {}

  /// Holds the error wrapped by `error`.
  template <class G>
    requires std::is_constructible_v<E, G&&>
  // NOLINTNEXTLINE(*-explicit-constructor): implicit, as in std
  constexpr ExpectedFallback(UnexpectedFallback<G> error)
      : storage_(std::in_place_index<1>, std::move(error).error()) {}

  /// True if a value is held.
  [[nodiscard]] constexpr bool has_value() const noexcept { return storage_.index() == 0; }
  /// True if a value is held.
  constexpr explicit operator bool() const noexcept { return has_value(); }

  /// The value. Precondition: `has_value()`.
  [[nodiscard]] constexpr const T& operator*() const& noexcept {
    return *std::get_if<0>(&storage_);
  }
  /// The value. Precondition: `has_value()`.
  [[nodiscard]] constexpr T& operator*() & noexcept { return *std::get_if<0>(&storage_); }
  /// The value, moved out. Precondition: `has_value()`.
  [[nodiscard]] constexpr T&& operator*() && noexcept {
    return std::move(*std::get_if<0>(&storage_));
  }
  /// Member access to the value. Precondition: `has_value()`.
  [[nodiscard]] constexpr const T* operator->() const noexcept { return std::get_if<0>(&storage_); }
  /// Member access to the value. Precondition: `has_value()`.
  [[nodiscard]] constexpr T* operator->() noexcept { return std::get_if<0>(&storage_); }

  /// The error. Precondition: `!has_value()`.
  [[nodiscard]] constexpr const E& error() const& noexcept { return *std::get_if<1>(&storage_); }
  /// The error. Precondition: `!has_value()`.
  [[nodiscard]] constexpr E& error() & noexcept { return *std::get_if<1>(&storage_); }

 private:
  std::variant<T, E> storage_;
};

}  // namespace detail

#if OB_USE_STD_EXPECTED
/// `std::expected<T, E>`.
template <class T, class E>
using expected = std::expected<T, E>;
/// `std::unexpected<E>`.
template <class E>
using unexpected = std::unexpected<E>;
#else
/// Fallback with the subset of the `std::expected<T, E>` interface used here.
template <class T, class E>
using expected = detail::ExpectedFallback<T, E>;
/// Fallback for `std::unexpected<E>`.
template <class E>
using unexpected = detail::UnexpectedFallback<E>;
#endif

/// Wraps `error` for returning from a function that returns `ob::expected`.
/// (Spelled as a function because deducing through alias templates needs a
/// newer compiler than C++20 support otherwise does.)
template <class E>
[[nodiscard]] constexpr unexpected<std::decay_t<E>> make_unexpected(E&& error) {
  return unexpected<std::decay_t<E>>(std::forward<E>(error));
}

}  // namespace ob
