// SPDX-License-Identifier: MIT
/// @file
/// A small deterministic random number generator.
///
/// `<random>` engines are specified exactly, but its distributions are not:
/// `std::uniform_int_distribution` may produce different sequences on
/// libstdc++ and libc++. Workloads and randomized tests must be reproducible
/// from a seed on every platform, so they use this generator and its integer
/// helpers instead.

#pragma once

#include <array>
#include <cassert>
#include <cstdint>

namespace ob {

/// xoshiro256** (Blackman and Vigna, "Scrambled linear pseudorandom number
/// generators", ACM TOMS 2021), seeded through SplitMix64 as its authors
/// recommend. Not cryptographically secure.
class Rng {
 public:
  /// Seeds the generator; equal seeds give equal sequences on every platform.
  explicit constexpr Rng(std::uint64_t seed) noexcept {
    for (std::uint64_t& word : state_) {
      seed += 0x9e3779b97f4a7c15ULL;  // SplitMix64
      std::uint64_t z = seed;
      z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
      word = z ^ (z >> 31U);
    }
  }

  /// The next 64 random bits.
  constexpr std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t t = state_[1] << 17U;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
  }

  /// Uniform integer in `[0, bound)`. Precondition: `bound > 0`.
  /// Debiased modulo reduction (see M. O'Neill, "Efficiently Generating a
  /// Number in a Range", 2018): exact, and integer-only.
  constexpr std::uint64_t below(std::uint64_t bound) noexcept {
    assert(bound > 0);
    const std::uint64_t threshold = (0 - bound) % bound;  // 2^64 mod bound
    for (;;) {
      const std::uint64_t value = next();
      if (value >= threshold) {
        return value % bound;
      }
    }
  }

  /// Uniform integer in `[low, high]`. Precondition: `low <= high`.
  constexpr std::int64_t between(std::int64_t low, std::int64_t high) noexcept {
    assert(low <= high);
    const auto span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
    if (span == UINT64_MAX) {
      return static_cast<std::int64_t>(next());
    }
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + below(span + 1));
  }

  /// True with probability `numerator / denominator`.
  constexpr bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return below(denominator) < numerator;
  }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, unsigned k) noexcept {
    return (x << k) | (x >> (64U - k));
  }

  std::array<std::uint64_t, 4> state_{};
};

}  // namespace ob
