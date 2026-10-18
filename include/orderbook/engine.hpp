// SPDX-License-Identifier: MIT
/// @file
/// `BasicEngine`: one book per symbol behind a single request interface.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <orderbook/book_api.hpp>
#include <orderbook/commands.hpp>
#include <orderbook/events.hpp>
#include <orderbook/order_book.hpp>
#include <orderbook/reference_book.hpp>
#include <orderbook/types.hpp>

namespace ob {

/// Routes requests to per-symbol books.
///
/// Symbols are registered with `add_symbol`, which hands out dense
/// `SymbolId`s. A request for an unregistered symbol is rejected with
/// `unknown_symbol`. Order ids only need to be unique per symbol, as with
/// exchanges that require the symbol on every cancel. Books never share
/// state, so an application can shard symbols across threads by giving
/// each thread its own engine.
template <OrderBookLike Book>
class BasicEngine {
 public:
  /// The book type.
  using book_type = Book;

  /// Creates an engine without symbols. Every book is created from
  /// `book_defaults` with its `symbol` field set to the new id.
  explicit BasicEngine(const BookConfig& book_defaults = {}) : defaults_(book_defaults) {}

  /// Registers `name` and returns its id; returns the existing id if `name`
  /// is already registered. Allocates; not for the hot path.
  SymbolId add_symbol(std::string_view name) {
    if (const std::optional<SymbolId> existing = find_symbol(name)) {
      return *existing;
    }
    const auto id = static_cast<SymbolId>(books_.size());
    BookConfig config = defaults_;
    config.symbol = id;
    books_.push_back(std::make_unique<Book>(config));
    names_.emplace_back(name);
    ids_.emplace(names_.back(), id);
    return id;
  }

  /// The id of `name`, if registered.
  [[nodiscard]] std::optional<SymbolId> find_symbol(std::string_view name) const {
    const auto it = ids_.find(name);
    if (it == ids_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Names of the registered symbols, indexed by `SymbolId`.
  [[nodiscard]] std::span<const std::string> symbol_names() const noexcept { return names_; }

  /// Number of registered symbols.
  [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }

  /// The book of `symbol`, or nullptr if it is not registered.
  [[nodiscard]] const Book* find_book(SymbolId symbol) const noexcept {
    const auto index = static_cast<std::size_t>(to_underlying(symbol));
    return index < books_.size() ? books_[index].get() : nullptr;
  }

  /// Submits a new order to the book of `order.symbol`.
  template <EventSink Sink>
  void submit(const NewOrder& order,
              Sink& sink) noexcept(noexcept(std::declval<Book&>().submit(order, sink))) {
    if (Book* book = book_of(order.symbol)) {
      book->submit(order, sink);
    } else {
      sink(Rejected{order.symbol, order.id, Request::new_order, RejectReason::unknown_symbol});
    }
  }

  /// Cancels a resting order of `request.symbol`.
  template <EventSink Sink>
  void cancel(const CancelOrder& request,
              Sink& sink) noexcept(noexcept(std::declval<Book&>().cancel(request, sink))) {
    if (Book* book = book_of(request.symbol)) {
      book->cancel(request, sink);
    } else {
      sink(Rejected{request.symbol, request.id, Request::cancel, RejectReason::unknown_symbol});
    }
  }

  /// Modifies a resting order of `request.symbol`.
  template <EventSink Sink>
  void modify(const ModifyOrder& request,
              Sink& sink) noexcept(noexcept(std::declval<Book&>().modify(request, sink))) {
    if (Book* book = book_of(request.symbol)) {
      book->modify(request, sink);
    } else {
      sink(Rejected{request.symbol, request.id, Request::modify, RejectReason::unknown_symbol});
    }
  }

  /// Dispatches any command. `noexcept` when the book's operations are.
  template <EventSink Sink>
  void apply(const Command& command, Sink& sink) noexcept(
      noexcept(std::declval<BasicEngine&>().submit(std::declval<const NewOrder&>(), sink)) &&
      noexcept(std::declval<BasicEngine&>().cancel(std::declval<const CancelOrder&>(), sink)) &&
      noexcept(std::declval<BasicEngine&>().modify(std::declval<const ModifyOrder&>(), sink))) {
    if (const auto* order = std::get_if<NewOrder>(&command)) {
      submit(*order, sink);
    } else if (const auto* cancel_request = std::get_if<CancelOrder>(&command)) {
      cancel(*cancel_request, sink);
    } else {
      modify(*std::get_if<ModifyOrder>(&command), sink);
    }
  }

 private:
  struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const noexcept {
      return std::hash<std::string_view>{}(name);
    }
  };

  [[nodiscard]] Book* book_of(SymbolId symbol) noexcept {
    const auto index = static_cast<std::size_t>(to_underlying(symbol));
    return index < books_.size() ? books_[index].get() : nullptr;
  }

  BookConfig defaults_;
  std::vector<std::unique_ptr<Book>> books_;
  std::vector<std::string> names_;
  std::unordered_map<std::string, SymbolId, NameHash, std::equal_to<>> ids_;
};

/// Engine over the optimised `OrderBook`.
using Engine = BasicEngine<OrderBook>;
/// Engine over the `std::map` baseline.
using MapEngine = BasicEngine<MapOrderBook>;
/// Engine over the reference implementation.
using ReferenceEngine = BasicEngine<ReferenceBook>;

}  // namespace ob
