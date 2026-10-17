// SPDX-License-Identifier: MIT
/// @file
/// `ob::expected<T, E>`: `std::expected` when the standard library provides it
/// (C++23, detected with the `__cpp_lib_expected` feature-test macro), and a
/// small fallback with the same spelling for the subset this library uses
/// otherwise. Code that sticks to `has_value()`, `operator bool`, `operator*`,
/// `operator->` and `error()` compiles unchanged against either.

#pragma once

#include <type_traits>
#include <utility>
#include <variant>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
/// 1 when `ob::expected` is `std::expected`, 0 when it is the fallback.
#define OB_HAS_STD_EXPECTED 1
#else
#define OB_HAS_STD_EXPECTED 0
#endif

namespace ob {

namespace detail {

/// Fallback for `std::unexpected`: wraps an error value.
template <class E>
class unexpected_fallback {
 public:
  /// Wraps `error`.
  constexpr explicit unexpected_fallback(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
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

/// True for specialisations of `unexpected_fallback`.
template <class T>
inline constexpr bool is_unexpected_fallback_v = false;
/// True for specialisations of `unexpected_fallback`.
template <class E>
inline constexpr bool is_unexpected_fallback_v<unexpected_fallback<E>> = true;

/// Fallback for `std::expected`: holds either a value or an error.
///
/// Accessing the value of an object that holds an error (or vice versa) is a
/// precondition violation, exactly as with `operator*` on `std::expected`.
template <class T, class E>
class expected_fallback {
 public:
  /// The value type.
  using value_type = T;
  /// The error type.
  using error_type = E;

  /// Holds `value`.
  template <class U = T>
    requires std::is_constructible_v<T, U&&> &&
             (!std::is_same_v<std::remove_cvref_t<U>, expected_fallback>) &&
             (!is_unexpected_fallback_v<std::remove_cvref_t<U>>)
  constexpr expected_fallback(U&& value)  // NOLINT(google-explicit-constructor): mirrors std
      : storage_(std::in_place_index<0>, std::forward<U>(value)) {}

  /// Holds the error wrapped by `error`.
  template <class G>
    requires std::is_constructible_v<E, G&&>
  constexpr expected_fallback(unexpected_fallback<G> error)  // NOLINT(google-explicit-constructor)
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

#if OB_HAS_STD_EXPECTED
/// `std::expected<T, E>`.
template <class T, class E>
using expected = std::expected<T, E>;
/// `std::unexpected<E>`.
template <class E>
using unexpected = std::unexpected<E>;
#else
/// Fallback with the subset of the `std::expected<T, E>` interface used here.
template <class T, class E>
using expected = detail::expected_fallback<T, E>;
/// Fallback for `std::unexpected<E>`.
template <class E>
using unexpected = detail::unexpected_fallback<E>;
#endif

/// Wraps `error` for returning from a function that returns `ob::expected`.
/// (Spelled as a function because deducing through alias templates needs a
/// newer compiler than C++20 support otherwise does.)
template <class E>
[[nodiscard]] constexpr unexpected<std::decay_t<E>> make_unexpected(E&& error) {
  return unexpected<std::decay_t<E>>(std::forward<E>(error));
}

}  // namespace ob
