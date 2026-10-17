// SPDX-License-Identifier: MIT
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <orderbook/book_api.hpp>
#include <orderbook/events.hpp>
#include <orderbook/format.hpp>
#include <orderbook/types.hpp>

#include "text.hpp"

namespace ob {
namespace {

/// Builds one `key=value` line.
class Line {
 public:
  Line(std::string& out, std::string_view kind, std::string_view symbol) : out_(out) {
    out_ += kind;
    out_ += ' ';
    out_ += symbol;
  }

  /// Terminates the line.
  void end() { out_ += '\n'; }

  Line& word(std::string_view text) {
    out_ += ' ';
    out_ += text;
    return *this;
  }

  Line& field(std::string_view key, std::string_view value) {
    out_ += ' ';
    out_ += key;
    out_ += '=';
    out_ += value;
    return *this;
  }

  template <class Int>
  Line& number(std::string_view key, Int value) {
    out_ += ' ';
    out_ += key;
    out_ += '=';
    detail::append_number(out_, value);
    return *this;
  }

 private:
  std::string& out_;
};

[[nodiscard]] std::string_view level_side(Side side) noexcept {
  return side == Side::buy ? "bid" : "ask";
}

/// Resolves symbol names; falls back to "#<id>" for unknown ids.
class SymbolNames {
 public:
  explicit SymbolNames(std::span<const std::string> names) noexcept : names_(names) {}

  [[nodiscard]] std::string_view operator()(SymbolId id) {
    const auto index = static_cast<std::size_t>(to_underlying(id));
    if (index < names_.size()) {
      return names_[index];
    }
    fallback_ = "#";
    detail::append_number(fallback_, to_underlying(id));
    return fallback_;
  }

 private:
  std::span<const std::string> names_;
  std::string fallback_;
};

}  // namespace

void append_event(std::string& out, const Event& event, std::span<const std::string> symbols) {
  SymbolNames name(symbols);
  if (const auto* e = std::get_if<Accepted>(&event)) {
    Line(out, "accepted", name(e->symbol))
        .number("id", to_underlying(e->id))
        .number("owner", to_underlying(e->owner))
        .field("side", to_string(e->side))
        .field("type", to_string(e->type))
        .number("price", e->price.value())
        .number("qty", e->qty.value())
        .end();
  } else if (const auto* r = std::get_if<Rejected>(&event)) {
    Line(out, "rejected", name(r->symbol))
        .number("id", to_underlying(r->id))
        .field("request", to_string(r->request))
        .field("reason", to_string(r->reason))
        .end();
  } else if (const auto* f = std::get_if<Fill>(&event)) {
    Line(out, "fill", name(f->symbol))
        .number("taker", to_underlying(f->taker_id))
        .number("maker", to_underlying(f->maker_id))
        .field("side", to_string(f->taker_side))
        .number("price", f->price.value())
        .number("qty", f->qty.value())
        .number("taker_left", f->taker_remaining.value())
        .number("maker_left", f->maker_remaining.value())
        .end();
  } else if (const auto* c = std::get_if<Cancelled>(&event)) {
    Line(out, "cancelled", name(c->symbol))
        .number("id", to_underlying(c->id))
        .number("qty", c->qty.value())
        .field("reason", to_string(c->reason))
        .end();
  } else if (const auto* m = std::get_if<Modified>(&event)) {
    Line(out, "modified", name(m->symbol))
        .number("id", to_underlying(m->id))
        .number("price", m->price.value())
        .number("qty", m->qty.value())
        .field("priority", m->priority_kept ? "kept" : "lost")
        .end();
  } else if (const auto* d = std::get_if<BookDelta>(&event)) {
    Line(out, "delta", name(d->symbol))
        .word(level_side(d->side))
        .number("price", d->price.value())
        .number("qty", d->qty.value())
        .number("orders", d->count)
        .end();
  }
}

void append_snapshot(std::string& out, std::string_view symbol, std::span<const LevelView> bids,
                     std::span<const LevelView> asks) {
  Line(out, "snapshot", symbol).number("bids", bids.size()).number("asks", asks.size()).end();
  const auto level = [&](Side side, const LevelView& view) {
    Line(out, "level", symbol)
        .word(level_side(side))
        .number("price", view.price.value())
        .number("qty", view.qty.value())
        .number("orders", view.count)
        .end();
  };
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
    level(Side::sell, *it);
  }
  for (const LevelView& view : bids) {
    level(Side::buy, view);
  }
}

}  // namespace ob
