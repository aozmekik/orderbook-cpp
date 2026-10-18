// SPDX-License-Identifier: MIT
// The benchmark latency histogram: bucketing and percentiles.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <orderbook/random.hpp>

#include "latency_histogram.hpp"

namespace ob::bench {
namespace {

TEST(LatencyHistogramTest, EmptyHistogramReportsZero) {
  const auto h = std::make_unique<LatencyHistogram>();
  EXPECT_EQ(h->count(), 0U);
  EXPECT_EQ(h->quantile(1, 2), 0U);
  EXPECT_EQ(h->min(), 0U);
  EXPECT_EQ(h->max(), 0U);
}

TEST(LatencyHistogramTest, SmallValuesAreExact) {
  const auto h = std::make_unique<LatencyHistogram>();
  for (std::uint64_t v = 1; v <= 100; ++v) {
    h->record(v);
  }
  EXPECT_EQ(h->quantile(1, 2), 50U);
  EXPECT_EQ(h->quantile(99, 100), 99U);
  EXPECT_EQ(h->quantile(1, 1), 100U);
  EXPECT_EQ(h->quantile(0, 1), 1U);
  EXPECT_EQ(h->min(), 1U);
  EXPECT_EQ(h->max(), 100U);
}

TEST(LatencyHistogramTest, BucketsAreContiguousAndBounded) {
  for (std::uint64_t v = 0; v < 1'000'000; v += 7) {
    const std::size_t index = LatencyHistogram::index_of(v);
    const std::uint64_t highest = LatencyHistogram::highest_in_bucket(index);
    ASSERT_GE(highest, v);
    ASSERT_LE(highest - v, (v / 64) + 1) << v;  // Within 1/64 relative error.
    ASSERT_EQ(LatencyHistogram::index_of(highest), index);
    ASSERT_EQ(LatencyHistogram::index_of(highest + 1), index + 1);
  }
  EXPECT_EQ(LatencyHistogram::highest_in_bucket(LatencyHistogram::index_of(UINT64_MAX)),
            UINT64_MAX);
}

TEST(LatencyHistogramTest, QuantilesMatchSortedDataWithinTheBucketError) {
  Rng rng(3);
  std::vector<std::uint64_t> values;
  const auto h = std::make_unique<LatencyHistogram>();
  for (int i = 0; i < 100'000; ++i) {
    // Heavy-tailed, like latencies: mostly small, occasionally huge.
    const std::uint64_t v = rng.chance(1, 100) ? rng.below(1'000'000) : 20 + rng.below(200);
    values.push_back(v);
    h->record(v);
  }
  std::ranges::sort(values);
  for (const auto [numerator, denominator] :
       {std::pair<std::uint64_t, std::uint64_t>{1, 2}, {9, 10}, {99, 100}, {999, 1000}}) {
    const std::size_t rank = ((values.size() * numerator) + denominator - 1) / denominator;
    const std::uint64_t exact = values[rank - 1];
    const std::uint64_t reported = h->quantile(numerator, denominator);
    EXPECT_GE(reported, exact);
    EXPECT_LE(reported - exact, (exact / 64) + 1) << numerator << "/" << denominator;
  }
  EXPECT_EQ(h->max(), values.back());
  EXPECT_EQ(h->quantile(1, 1), values.back());
}

}  // namespace
}  // namespace ob::bench
