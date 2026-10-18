// SPDX-License-Identifier: MIT
// Verifies that a warmed-up OrderBook does not touch the heap while it
// processes requests. This binary replaces the global allocation functions
// with counting versions, so it is kept separate from the other tests.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

#include <gtest/gtest.h>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/engine.hpp>
#include <orderbook/events.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/workload.hpp>

namespace {

// The replaced allocation functions must reach these, so they are globals.
std::atomic<std::uint64_t> g_allocations{0};  // NOLINT(*-avoid-non-const-global-variables)
std::atomic<bool> g_counting{false};          // NOLINT(*-avoid-non-const-global-variables)

void* counted_allocate(std::size_t size, std::size_t alignment) {
  if (g_counting.load(std::memory_order_relaxed)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  void* memory = nullptr;
  const std::size_t bytes = size == 0 ? 1 : size;
  if (alignment <= alignof(std::max_align_t)) {
    memory = std::malloc(bytes);  // NOLINT(cppcoreguidelines-no-malloc,*-owning-memory)
  } else if (posix_memalign(&memory, alignment, bytes) != 0) {
    memory = nullptr;
  }
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  return memory;
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-use-internal-linkage)
void* operator new(std::size_t size) {
  return counted_allocate(size, 0);
}
void* operator new[](std::size_t size) {
  return counted_allocate(size, 0);
}
void* operator new(std::size_t size, std::align_val_t align) {
  return counted_allocate(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return counted_allocate(size, static_cast<std::size_t>(align));
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  try {
    return counted_allocate(size, 0);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  try {
    return counted_allocate(size, 0);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t size, std::align_val_t align,
                   const std::nothrow_t& /*tag*/) noexcept {
  try {
    return counted_allocate(size, static_cast<std::size_t>(align));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t align,
                     const std::nothrow_t& /*tag*/) noexcept {
  try {
    return counted_allocate(size, static_cast<std::size_t>(align));
  } catch (...) {
    return nullptr;
  }
}
// Every replaceable deallocation function is defined too, so that no
// variant from the standard library (or a sanitizer runtime) is mixed in.
void operator delete(void* memory) noexcept {
  std::free(memory);
}
void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::align_val_t /*align*/,
                     const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::align_val_t /*align*/,
                       const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::size_t /*size*/) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::size_t /*size*/) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::align_val_t /*align*/) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::align_val_t /*align*/) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
  std::free(memory);
}
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-use-internal-linkage)

namespace ob {
namespace {

// Rejections are events and the hot path never throws.
static_assert(noexcept(std::declval<OrderBook&>().submit(std::declval<const NewOrder&>(),
                                                         std::declval<NullSink&>())));
static_assert(noexcept(std::declval<OrderBook&>().cancel(std::declval<const CancelOrder&>(),
                                                         std::declval<NullSink&>())));
static_assert(noexcept(std::declval<OrderBook&>().modify(std::declval<const ModifyOrder&>(),
                                                         std::declval<NullSink&>())));

/// Allocations made while replaying `w` through a fresh engine whose book
/// has room for `reserve` orders.
template <class EngineT>
std::uint64_t allocations_during_replay(const workload::Workload& w, std::uint32_t reserve) {
  EngineT engine(BookConfig{.initial_capacity = reserve});
  engine.add_symbol(w.symbols.front());
  NullSink sink;
  g_allocations.store(0);
  g_counting.store(true);
  for (const Command& command : w.commands) {
    engine.apply(command, sink);
  }
  g_counting.store(false);
  return g_allocations.load();
}

// Debug builds check every invariant after every request (O(book size)),
// so they replay a shorter stream over a smaller book.
constexpr bool kBooksSelfCheck = OB_ENABLE_INVARIANT_CHECKS != 0;

workload::Workload make_workload() {
  workload::Config config;
  config.commands = kBooksSelfCheck ? 50'000 : 300'000;
  config.target_orders = kBooksSelfCheck ? 500 : 2'000;
  return workload::generate(config);
}

TEST(AllocationTest, CountingAllocatorSeesAllocations) {
  g_allocations.store(0);
  g_counting.store(true);
  // Call the allocation function directly: a new-expression may be elided.
  void* probe = ::operator new(16);
  g_counting.store(false);
  ::operator delete(probe);
  EXPECT_EQ(g_allocations.load(), 1U);
}

TEST(AllocationTest, ReservedOrderBookNeverAllocates) {
  const workload::Workload w = make_workload();
  ASSERT_LT(w.stats.live_at_end, 16'384U);
  EXPECT_EQ(allocations_during_replay<Engine>(w, 16'384), 0U);
}

TEST(AllocationTest, MapBaselineAllocatesForNewPriceLevels) {
  // The std::map baseline allocates a tree node for every new price level;
  // this is the difference the ladder removes.
  const workload::Workload w = make_workload();
  EXPECT_GT(allocations_during_replay<MapEngine>(w, 16'384), 100U);
}

TEST(AllocationTest, SmallInitialCapacityOnlyAllocatesWhileGrowing) {
  const workload::Workload w = make_workload();
  const std::uint64_t allocations = allocations_during_replay<Engine>(w, 16);
  EXPECT_GT(allocations, 0U);
  EXPECT_LT(allocations, 100U);  // Geometric growth: a few dozen reallocations.
}

}  // namespace
}  // namespace ob
