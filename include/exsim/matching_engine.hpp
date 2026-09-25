// Multi-symbol front end: routes decoded commands to per-symbol books.
#pragma once

#include <memory>
#include <vector>

#include "exsim/common.hpp"
#include "exsim/order_book.hpp"
#include "exsim/reference_book.hpp"

namespace exsim {

// Books are fully independent: each owns its ladder, order store and id index, and order ids only
// need to be unique among a symbol's live orders. Symbols can therefore be sharded across matching
// threads without shared state. This engine is one shard, driven by a single thread.
template <class Book>
class MatchingEngine {
 public:
  using book_type = Book;

  MatchingEngine(std::uint32_t num_symbols, const BookConfig& cfg) {
    books_.reserve(num_symbols);
    for (std::uint32_t i = 0; i < num_symbols; ++i) books_.push_back(std::make_unique<Book>(i, cfg));
  }

  template <class Sink>
  EXSIM_ALWAYS_INLINE void process(const Command& c, Sink& sink) {
    if (EXSIM_UNLIKELY(c.symbol >= books_.size())) return sink.on_event(rejected_event(c, Reason::UnknownSymbol));
    Book& b = *books_[c.symbol];
    switch (c.type) {
      case MsgType::NewOrder: return b.add(c, sink);
      case MsgType::Cancel: return b.cancel(c, sink);
      case MsgType::Modify: return b.modify(c, sink);
    }
  }

  Book& book(SymbolId s) { return *books_[s]; }
  const Book& book(SymbolId s) const { return *books_[s]; }
  std::uint32_t num_symbols() const { return static_cast<std::uint32_t>(books_.size()); }

 private:
  std::vector<std::unique_ptr<Book>> books_;
};

using AosScatterBook = OrderBook<AosStore, ScatterHash>;  // design point 1
using AosBook = OrderBook<AosStore>;
using SoaBook = OrderBook<SoaStore>;
using HybridBook = OrderBook<HybridStore>;
using DefaultBook = AosBook;  // fewest simulated cache misses; wall-clock ties with hybrid/soa (BENCHMARKS.md)

}  // namespace exsim
