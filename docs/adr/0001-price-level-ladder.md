# ADR 0001: Price levels in a tick-indexed ring around the touch

- Status: accepted
- Date: 2026-10-18

## Context

Each side of a book maps prices to price levels. The operations, roughly in
order of frequency on real order flow, are:

1. find the level of a price, creating it if needed (every resting order);
2. remove an empty level (cancels and fills);
3. find the best level, and the next one after it empties (matching);
4. iterate levels from the best (depth snapshots, fill-or-kill checks).

Order flow is concentrated near the touch: studies of limit order books
(Bouchaud, Mezard and Potters 2002; Cont, Stoikov and Talreja 2010) find
that the arrival rate of limit orders decays like a power law with the
distance from the best quote, and most orders are cancelled close to where
they were placed. Prices are integers (ticks), so "near the touch" means a
small integer range. Prices far away are rare but must work, and the touch
moves, sometimes far (a gap, a trending market).

## Options considered

1. **`std::map<Price, Level>`** (red-black tree). O(log n) everything, one
   heap allocation per new level, pointer chasing on every lookup. Simple
   and the usual baseline.
2. **Sorted `std::vector` with the best level at the end**, searched
   linearly from the end. Very cache-friendly; inserting or removing a level
   at distance d from the touch moves d elements. A good choice when books
   are shallow; degrades with deep books and inserts far from the touch.
3. **Dense array over the whole valid price range**, plus a hierarchical
   bitmap to find the next level. O(1) and O(log64 n), but the memory is
   proportional to the price range (2^63 ticks here), so it only works when
   the venue imposes narrow price bands.
4. **Dense ring over a window of W ticks around the touch, with a sorted
   overflow for the rest** (chosen). Near-touch work is an array index and a
   bit operation; memory is fixed (W slots per side); prices outside the
   window still work.

## Decision

`LadderLevels` stores the levels whose key lies in `[base, base + W)` in a
ring of `W` slots indexed by `key mod W` (W a power of two, 4096 by
default), with one occupancy bit per slot. Keys are prices for asks and
negated prices for bids, so "best" is the minimum on both sides and one
implementation serves both. Levels at or past `base + W` live in a vector
sorted from worst to best, so the one nearest the window is at the back.

The window follows the touch with hysteresis:

- a key better than `base` moves the window down so that the key sits W/8
  above `base`, and the far end of the ring moves to the overflow;
- when the best key drifts W/2 above `base`, the window moves up to restore
  W/8 of headroom and pulls overflow levels that now fit, but only once as
  many operations have passed since the previous window move as that move
  touched levels;
- emptying the ring re-centres the window on the best overflow level, so the
  ring is never empty while the overflow is not.

Because the slot of a key is `key mod W`, moving the window only touches the
levels that enter or leave it; the rest stay where they are. Finding the
next level after the best one empties is a scan over 64-bit occupancy words,
which is one word for the common case of a neighbouring tick.

`BasicOrderBook` takes the level index as a template parameter. `MapLevels`
(option 1) is kept as a first-class alternative so that the benchmarks
compare the level structure alone, with everything else (pools, queues, id
map, event code) identical.

## Consequences

- Inserts, lookups and removals near the touch are O(1) and allocation-free;
  the ring and bitmap are allocated once per side (4096 x 4 bytes plus 512
  bytes of bitmap by default).
- Levels outside the window cost O(log n) to find and O(n) to insert or
  remove in the overflow vector. A workload that places many orders far from
  the touch pays for that; the `WorkloadWide` benchmark measures it.
- Window moves cost O(levels moved + W/64). A move down is forced (the best
  key must be in the ring); a move up is voluntary, and the delay above
  makes moves amortised O(1) per operation for a stream that alternates
  between a price far better than the best and the old touch. Without the
  delay, toggling one order 3W/8 or more ticks better than the best of a
  dense book moved the window down and back up on every request: with
  W = 4096 and 3,000 ask levels, about 4.5 us of CPU time per request at
  1,600 ticks and 10.8 us at 3,000 ticks, against about 45 ns for
  `std::map`. With it, those cases cost 44 ns and 64 ns per request on
  average; the single request that does move the window still pays
  O(levels moved). Toggling by about W or more empties the ring, which
  forces a move on every request (13.8 us each in the same book). The
  `WindowToggle` benchmarks measure these cases; docs/benchmarks.md has
  the numbers and how they were taken.
- The overflow is a sorted vector, so inserting into or erasing from it
  moves O(n) entries. With 100,000 levels beyond the window, an add or a
  cancel there costs about 15.6 us against 152 ns for `std::map` (the
  `DeepOverflow` benchmark). Books whose activity lives far from the touch
  should use a wider window or `MapOrderBook`; a better far-from-touch
  structure is on the roadmap.
- The invariants (bitmap matches the slots, the best key is the minimum, the
  overflow is sorted and outside the window, ring empty implies overflow
  empty) are checked by `LadderLevels::check_invariants`, after every book
  operation in this project's debug builds, and the index is tested against a
  `std::map` model with windows as small as 64 ticks.
- W is a tuning knob (`BookConfig::ladder_ticks`). It should cover the
  price range where most of the activity happens; it does not limit the
  prices a book accepts.
