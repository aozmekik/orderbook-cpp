# ADR 0004: Correctness by differential testing against a reference book

- Status: accepted
- Date: 2026-10-18

## Context

The optimised book is full of state that must agree with itself: a ring and
a bitmap, an overflow vector, a window that moves, intrusive links,
recycled slots, a hash table with backward-shift deletion. Example-based
tests catch the cases their author thought of; the bugs in this kind of code
live in combinations (a window shift while a level empties during a
self-trade cancel, say).

## Decision

1. Keep a **reference book** (`ReferenceBook`) written for obviousness:
   `std::map` of `std::list` per side, prices compared directly, totals
   recomputed on demand, and an order found by looking up its level in a
   `std::unordered_map` and then searching that level's list linearly. It
   shares only the stateless request validation with the optimised book.
2. **Differential tests**: replay long random request streams through the
   ladder book, the `std::map` baseline and the reference book, and require
   identical event vectors after every request, plus identical depth
   snapshots every 1024 requests. The stream is adversarial: all order types,
   four owners under every self-trade policy, duplicate, unknown and zero ids,
   invalid fields, prices at the ends of the valid range, and jumps far past
   the ladder window (run with windows of 64 and 4096 ticks). 24
   configurations, with fixed seeds for the streams and for the books'
   id-map hash, so that a failure reproduces.
3. **Invariant checks** after every request in this project's unoptimised
   builds (`OB_ENABLE_INVARIANT_CHECKS=1`, set by CMake for its own tests,
   tools and examples; off by default for consumers): the book is not
   crossed, every level's total and count equal the sum over its queue, the
   links, pools, id map and level index agree, and quantity is conserved.
   The differential tests also call the checks explicitly in release builds.
4. **Fuzzing**: `fuzz_engine` feeds arbitrary bytes, parsed as an order
   stream, to all three books and aborts on any difference in their events
   or any invariant violation after any command; the fuzz preset keeps
   assertions on. `fuzz_csv` checks the parser.
5. **Golden files**: `obreplay` output for the examples is checked in and
   compared byte for byte for every engine.

## Consequences

- A change to the optimised book is checked against an independent
  implementation of the same semantics on millions of requests in a few
  seconds; a semantic change has to be made in both books, deliberately.
- The reference book is slow on purpose (its cancel is linear in the size of
  the level) and is not meant for production use; it is benchmarked only to
  show what the optimisations buy.
- Both books could share a misunderstanding of the semantics. The typed
  tests in `tests/book_test.cpp` pin the intended event sequences
  independently of either implementation, and `docs/semantics.md` states
  them in prose.
- Debug builds run fewer random requests (the invariant check is
  O(book size) per request); release builds run the full count, and
  `OB_DIFF_OPS` raises it for a longer run.
