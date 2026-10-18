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
  under the constant insert/erase churn of a book. Id 0 marks an empty slot,
  which is why order id 0 is invalid.
- The home slot of an id is the top bits of `fmix64(id) * m`: the MurmurHash3
  finalizer spreads sequential ids, and `m` is an odd multiplier derived from
  a per-book seed (`BookConfig::hash_seed`, random by default), which makes
  the slot a multiply-shift hash (Dietzfelbinger et al., 1997). Ids are
  chosen by clients and fmix64 is public and invertible: without the seed a
  client could choose ids that all share one slot, and every request would
  scan the table. The seed is not a cryptographic key; a venue exposed to
  untrusted clients should still map their ids to its own at the gateway.
- Pools, the map and the ladder's overflow grow geometrically when full,
  like `std::vector`. `BookConfig::initial_capacity` (or `reserve`) sizes all
  of them up front.

## Consequences

- An `OrderBook` that stays within its reserved capacity performs no heap
  allocation while it processes requests, wherever its price levels are.
  `tests/allocation_test.cpp` replaces the global allocator and checks that
  on the near-touch and the wide synthetic workloads, with 200 levels outside
  the ladder window, after a window move that evicts 500 levels and after
  `reserve`; it also checks that the `std::map` baseline does allocate.
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
