# Matching semantics

This is the contract that `OrderBook`, `MapOrderBook` and `ReferenceBook`
implement. The differential tests and the `fuzz_engine` target check that all
three emit exactly the same events for the same requests, and the typed tests
in [`tests/book_test.cpp`](../tests/book_test.cpp) pin the event sequences
listed below.

## Units and limits

| Quantity | Type | Valid range |
| --- | --- | --- |
| Price | `Price`, signed 64-bit ticks | `[-2^62, 2^62]` (`kMinPrice`, `kMaxPrice`) |
| Quantity | `Qty`, signed 64-bit lots | `[1, 2^31 - 1]` (`kMaxOrderQty`) |
| Order id | `OrderId`, unsigned 64-bit | non-zero |
| Owner | `OwnerId`, unsigned 32-bit | any; `0` is anonymous |

There is no floating point anywhere in matching. The price range leaves room
to negate a price and to offset it by a ladder window without overflow; the
quantity limit, together with the 32-bit order handles, means that any sum of
open quantities in a book fits in a signed 64-bit integer.

Order ids only need to be unique among the **live** (resting) orders of one
book. Once an order is filled or cancelled its id may be reused, and two
symbols may use the same ids.

## Order types

| Type | Matches | Remainder |
| --- | --- | --- |
| `limit` | at or better than its price | rests (good till cancelled) |
| `market` | at any price | cancelled (`ioc_remainder`) |
| `ioc` | at or better than its price | cancelled (`ioc_remainder`) |
| `fok` | at or better than its price, only if the whole quantity can fill now | none: fills completely or is cancelled whole (`fok_unfillable`) without touching the book |
| `post_only` | never | rests; if it would match on arrival it is **rejected** (`post_only_would_cross`) |

Matching is price-time priority: the best price first; within a price level,
the order that has rested longest first. Trades happen at the resting
(maker) order's price, so an aggressive limit order gets price improvement.

Post-only orders are rejected rather than repriced (see
[ADR 0003](adr/0003-order-semantics.md)). "Would match" includes touching the
best opposite price.

## Self-trade prevention

`BookConfig::stp` applies when an incoming order meets a resting order with
the same non-zero owner:

- `none`: they trade.
- `cancel_newest`: the incoming order's remaining quantity is cancelled
  (`self_trade`); fills that already happened stand and the resting order
  stays.
- `cancel_oldest`: the resting order is cancelled (`self_trade`) and matching
  continues with the next resting order.

A fill-or-kill order counts only the liquidity it could actually take under
the policy: under `cancel_newest`, what rests ahead of the first own order;
under `cancel_oldest`, everything except own orders. If that is not enough
the order is killed and nothing in the book changes, so own orders are not
cancelled by a killed FOK.

## Cancel and modify

`cancel` removes a resting order and reports its open quantity as
`Cancelled{reason = requested}`.

`modify(id, price, qty)` sets the new **open** quantity (not the original
quantity) and follows exchange priority rules:

| Change | Priority | Notes |
| --- | --- | --- |
| same price, smaller or equal size | kept | the order is resized in place |
| same price, larger size | lost | the order moves to the back of its level |
| new price | lost | leaves its level, then behaves like a new order with the same id, type and owner: it may match (and self-trade prevention applies), and the remainder rests |

A post-only order whose new price would match is rejected and left as it was.

## Validation and rejections

A rejected request has no effect. Checks run in this order:

- New order: id is non-zero (`invalid_order_id`); quantity is valid
  (`invalid_quantity`); price is valid unless it is a market order
  (`invalid_price`); the id is not live (`duplicate_order_id`); for `limit`
  and `post_only`, the book is not full (`book_full`); for `post_only`, it
  would not match (`post_only_would_cross`).
- Cancel: the id is live (`unknown_order`).
- Modify: quantity, then price, are valid; then the id is live
  (`unknown_order`); then the post-only rule above.
- Engine: the symbol is registered (`unknown_symbol`), before any of the
  above.

`book_full` is checked when the order arrives, even if it would have filled
completely; orders that never rest (`market`, `ioc`, `fok`) are not limited.

## Events and their order

Every request emits its events synchronously, in this order:

1. `Rejected`, and nothing else; or
2. for a new order, `Accepted` (market orders report price 0); for a modify,
   `Modified` (with `priority_kept`);
3. if the modify changed the price: the `BookDelta` of the level the order
   left;
4. matching, level by level from the best price: a `Fill` per maker that
   traded and a `Cancelled` per maker removed by `cancel_oldest`, in queue
   order, then one `BookDelta` for that level if anything on it changed;
5. the end of the incoming order: either a `BookDelta` for the level where
   its remainder rests, or a `Cancelled` for its remainder (`ioc_remainder`,
   `fok_unfillable` or `self_trade`), or nothing if it filled completely.

For a cancel: `Cancelled`, then the `BookDelta` of the level. For a resize at
the same price: `Modified`, then the `BookDelta` (omitted when nothing
changed).

A `BookDelta` carries the level's new total quantity and order count; zero
means the level is gone. The deltas alone are enough to maintain a level-2
book, which [`examples/market_data.cpp`](../examples/market_data.cpp)
demonstrates.

Accounting: every accepted order's quantity is eventually accounted for by
its fills, at most one `Cancelled`, and what still rests. With
`OB_ENABLE_INVARIANT_CHECKS=1` (this project's own unoptimised builds), each
book checks the corresponding conservation law (accepted + increases = 2 x
filled + cancelled + decreases + resting) after every request.

## Determinism

Nothing a book emits depends on time, addresses, hash-table iteration order
or the platform's random number generator. The one random input of a book,
the seed of its order-id hash table (`BookConfig::hash_seed`, drawn from
`std::random_device` unless set), decides only where ids sit in that table,
which is never iterated. The same requests always produce the same events, and `ob::append_event` renders them without locale or
formatting state, so `obreplay` output is byte-identical between runs and
between the three book implementations (`tests/golden/`).
