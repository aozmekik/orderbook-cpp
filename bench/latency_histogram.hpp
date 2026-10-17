// SPDX-License-Identifier: MIT
// A fixed-size log-linear histogram in the style of HdrHistogram (Gil Tene),
// used by the benchmarks to report latency percentiles.
#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ob::bench {

/// Records non-negative integers (nanoseconds, here) with bounded relative
/// error. Values below 2^kSubBits are counted exactly; above that, every
/// power-of-two range is split into 2^(kSubBits-1) equal buckets, so a
/// reported value is within 2^-(kSubBits-1) (about 1.6%) of the recorded one.
/// Recording is O(1) and never allocates.
class LatencyHistogram {
 public:
  /// Precision: 7 bits gives 64 buckets per power of two.
  static constexpr unsigned kSubBits = 7;

  /// Counts one occurrence of `value`.
  void record(std::uint64_t value) noexcept {
    ++counts_[index_of(value)];
    ++total_;
    min_ = value < min_ ? value : min_;
    max_ = value > max_ ? value : max_;
  }

  /// Number of recorded values.
  [[nodiscard]] std::uint64_t count() const noexcept { return total_; }

  /// Smallest recorded value (exact); 0 if empty.
  [[nodiscard]] std::uint64_t min() const noexcept { return total_ == 0 ? 0 : min_; }

  /// Largest recorded value (exact); 0 if empty.
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

  /// The value at quantile `numerator / denominator` (for example 999/1000
  /// for p99.9): the highest value that falls in the same bucket as the
  /// recorded value of that rank, capped at the exact maximum. 0 if empty.
  [[nodiscard]] std::uint64_t quantile(std::uint64_t numerator,
                                       std::uint64_t denominator) const noexcept {
    assert(denominator > 0 && numerator <= denominator);
    if (total_ == 0) {
      return 0;
    }
    // Rank of the wanted value, 1-based, rounding up (nearest-rank method).
    const std::uint64_t rank =
        numerator == 0 ? 1
                       : (total_ / denominator) * numerator +
                             ((total_ % denominator) * numerator + denominator - 1) / denominator;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen >= rank && counts_[i] > 0) {
        const std::uint64_t highest = highest_in_bucket(i);
        return highest < max_ ? highest : max_;
      }
    }
    return max_;
  }

  /// Bucket of `value` (exposed for tests).
  [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t value) noexcept {
    if (value < kExact) {
      return static_cast<std::size_t>(value);
    }
    const auto shift = static_cast<unsigned>(std::bit_width(value)) - kSubBits;
    const std::uint64_t sub = (value >> shift) - kHalf;
    return static_cast<std::size_t>(kExact + (shift - 1) * kHalf + sub);
  }

  /// Largest value that maps to bucket `index` (exposed for tests).
  [[nodiscard]] static constexpr std::uint64_t highest_in_bucket(std::size_t index) noexcept {
    if (index < kExact) {
      return index;
    }
    const std::uint64_t shift = (index - kExact) / kHalf + 1;
    const std::uint64_t sub = (index - kExact) % kHalf;
    const std::uint64_t lowest = (kHalf + sub) << shift;
    return lowest + ((std::uint64_t{1} << shift) - 1);
  }

 private:
  static constexpr std::uint64_t kExact = std::uint64_t{1} << kSubBits;
  static constexpr std::uint64_t kHalf = kExact / 2;
  static constexpr std::size_t kBuckets = kExact + (64 - kSubBits) * kHalf;

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace ob::bench
