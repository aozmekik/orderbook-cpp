# ADR 0002: Orders in index-addressed pools with intrusive FIFO queues

- Status: accepted
- Date: 2026-10-18

## Context

A matching engine creates and destroys an order for almost every request,
and most of them are destroyed by a cancel that arrives in the middle of a
queue. The hot path should not call the allocator, should not chase
pointers through scattered heap nodes, and must find an order by id in O(1).

## Options considered

1. `std::list` per level plus `std::unordered_map<OrderId, iterator>`. Every
   order is a separate allocation, and so is every map node.
2. Heap-allocated nodes with intrusive `prev`/`next` pointers, and a custom
   allocator or free list. No per-order `new`, but 8-byte links and stable
   addresses tie the design to one arena.
3. **All orders in one contiguous pool addressed by 32-bit indices, with
   intrusive `prev`/`next` indices, a LIFO free list, and an open-addressing
   id map** (chosen).

## Decision

- `detail::Pool<T>` keeps objects in a `std::vector` and recycles freed
  slots last-in first-out, so a new order usually reuses a slot that is still
  in cache. Handles are 32-bit indices, which halves the size of every link
  compared with pointers. An `OrderNode` is 40 bytes; a `Level` is 32.
- Each price level is a doubly linked FIFO threaded through the order nodes
  (`head`, `tail`, `total`, `count` live in the level). Appending, popping
  the front and unlinking from the middle are O(1).
- `detail::OrderIdMap` maps order ids to node indices with linear probing in
  a power-of-two table at most half full. Deletion uses backward shifting
  (Knuth's Algorithm R) instead of tombstones, so probe lengths do not decay
  under the constant insert/erase churn of a book. Keys are mixed with the
  MurmurHash3 finalizer because ids are usually sequential. Id 0 marks an
  empty slot, which is why order id 0 is invalid.
- Pools and the map grow geometrically when full, like `std::vector`.
  `BookConfig::initial_capacity` (or `reserve`) sizes them up front.

## Consequences

- A book that stays within its reserved capacity performs no heap allocation
  while it processes requests. `tests/allocation_test.cpp` replaces the
  global allocator and checks exactly that on a synthetic workload (and that
  the `std::map` baseline does allocate).
- Growth is a reallocation and a copy (or a rehash) of the whole pool, a
  latency spike proportional to the book size. Production systems would
  reserve for the worst case; the default reserves 1024 orders.
- Indices instead of pointers mean no reference to a node may be held across
  an `allocate`. The book only allocates when an order comes to rest, after
  matching, which is where this matters.
- Allocation failure inside the `noexcept` operations terminates the
  process. That is deliberate: there is no meaningful recovery from running
  out of memory in the middle of a match, and it keeps exceptions off the
  hot path.
- Indices limit a book to 2^32 - 2 resting orders; with quantities capped at
  2^31 - 1, any sum of open quantity fits in 64 bits.
