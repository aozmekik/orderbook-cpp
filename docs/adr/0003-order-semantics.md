# ADR 0003: Order semantics where venues differ

- Status: accepted
- Date: 2026-10-18

## Context

Exchanges agree on price-time priority but differ on the details below.
Each choice changes the event stream, so it has to be made once and
implemented identically by every book (the differential tests compare the
optimised book with the reference book event by event).

## Decisions

### Post-only orders are rejected, not repriced

Options: reject a post-only order that would take liquidity, or reprice it
one tick behind the opposite best ("slide"). Rejecting is simpler to reason
about (the order either rests at the price the client chose or does not
exist), never shows the market a price the client did not send, and is what
several venues do by default (for example Binance's `LIMIT_MAKER`). A post-
only order that touches the opposite best counts as crossing. A modify that
would make a resting post-only order cross is rejected and leaves the order
unchanged.

### Modify sets the new open quantity

Options: the new total (original) quantity, as in FIX `OrderQty`, or the new
open quantity. The book does not track filled quantity per order, and the
open quantity is what the client controls after partial fills, so `modify`
takes the open quantity. Reducing it at the same price keeps priority;
increasing it or changing the price loses priority. A price change behaves
like cancel-and-new with the same id, including matching if it crosses.

### Self-trade prevention is a per-book policy

Options: a per-order instruction (as on CME or Binance) or a book-wide
setting. A book-wide setting keeps the request types and the CSV format
small. `cancel_newest` cancels the incoming remainder; `cancel_oldest`
cancels the resting orders and keeps matching. Owner 0 is anonymous and never
self-trades, so streams without owners behave as if prevention were off. A
per-order policy is on the roadmap.

### Fill-or-kill is checked before it touches the book

A FOK order first computes the quantity it could take under the current
self-trade policy; if that is short, it is cancelled whole and nothing else
happens. It never partially executes and never cancels own resting orders
when it is killed.

### Market, IOC and FOK orders are accepted, then cancelled

Every valid new order produces `Accepted` first, so a consumer can rely on
one rule: an accepted order's quantity is accounted for by fills, at most
one `Cancelled`, and what rests. Only invalid requests and post-only orders
that would cross produce `Rejected`.

### `book_full` is checked on arrival

When `max_orders` orders rest, a new `limit` or `post_only` order is
rejected even if it would have filled completely; orders that never rest are
not limited. Checking on arrival keeps the decision independent of how the
order would have matched.

## Consequences

These rules are written down in [docs/semantics.md](../semantics.md) and
pinned by the typed tests in `tests/book_test.cpp`, which run against all
three book implementations.
