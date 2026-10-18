// SPDX-License-Identifier: MIT
#include <cstdint>
#include <random>

#include <orderbook/detail/order_id_map.hpp>

namespace ob::detail {

std::uint64_t random_hash_seed() {
  std::random_device device;
  // result_type is unsigned int; take 32 bits from each of two draws.
  const std::uint64_t high = device() & 0xffff'ffffU;
  const std::uint64_t low = device() & 0xffff'ffffU;
  return (high << 32U) | low;
}

}  // namespace ob::detail
