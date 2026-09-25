// Shared test utilities: command builders, a book harness, and multi-implementation test registration.
#pragma once

#include <cstring>
#include <vector>

#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"
#include "test_framework.hpp"

namespace exsim::test {

inline Command new_order(OrderId id, Side side, Price px, Qty qty, Tif tif = Tif::Day,
                         OrdType type = OrdType::Limit, std::uint8_t flags = 0, OwnerId owner = 1) {
  Command c{};
  c.type = MsgType::NewOrder;
  c.order_id = id, c.side = side, c.price = px, c.qty = qty, c.tif = tif, c.ord_type = type, c.flags = flags;
  c.owner = owner;
  return c;
}
inline Command market(OrderId id, Side side, Qty qty, OwnerId owner = 1) {
  return new_order(id, side, 0, qty, Tif::Day, OrdType::Market, 0, owner);
}
inline Command cancel(OrderId id) {
  Command c{};
  c.type = MsgType::Cancel;
  c.order_id = id;
  return c;
}
inline Command modify(OrderId id, Price px, Qty qty) {
  Command c{};
  c.type = MsgType::Modify;
  c.order_id = id, c.price = px, c.qty = qty;
  return c;
}

// Price band [1000, 2023], 64 resting orders max.
inline BookConfig small_config(Stp stp = Stp::None) {
  BookConfig cfg;
  cfg.min_price = 1000;
  cfg.num_levels = 1024;
  cfg.max_orders = 64;
  cfg.stp = stp;
  return cfg;
}

template <class Book>
struct Harness {
  explicit Harness(const BookConfig& cfg = small_config()) : book(0, cfg) {}

  std::vector<Event> send(const Command& c) {
    sink.events.clear();
    switch (c.type) {
      case MsgType::NewOrder: book.add(c, sink); break;
      case MsgType::Cancel: book.cancel(c, sink); break;
      case MsgType::Modify: book.modify(c, sink); break;
    }
    return sink.events;
  }

  Book book;
  VectorSink sink;
};

inline bool same_bytes(const Event& a, const Event& b) { return std::memcmp(&a, &b, sizeof(Event)) == 0; }

}  // namespace exsim::test

// Registers one test case per book implementation from a single templated body.
#define BOOK_TEST(name)                                         \
  template <class Book>                                         \
  static void name();                                           \
  TEST(name##__ref) { name<::exsim::ReferenceBook>(); }         \
  TEST(name##__aos_scatter) { name<::exsim::AosScatterBook>(); } \
  TEST(name##__aos) { name<::exsim::AosBook>(); }               \
  TEST(name##__soa) { name<::exsim::SoaBook>(); }               \
  TEST(name##__hybrid) { name<::exsim::HybridBook>(); }         \
  template <class Book>                                         \
  static void name()

// Field-by-field event assertions. The event expression is evaluated exactly once (it usually sends a
// command), then compared field by field.
#define CHECK_ACCEPTED(e, id_, side_, px_, qty_)           do {                                                       const ::exsim::Event ev_ = (e);                          CHECK_EQ(ev_.type, ::exsim::EventType::Accepted);        CHECK_EQ(ev_.order_id, OrderId{id_});                    CHECK(ev_.side == (side_));                              CHECK_EQ(ev_.price, Price{px_});                         CHECK_EQ(ev_.qty, Qty{qty_});                          } while (0)

#define CHECK_TRADE(e, taker_, maker_, px_, qty_, leaves_)   do {                                                         const ::exsim::Event ev_ = (e);                            CHECK_EQ(ev_.type, ::exsim::EventType::Trade);             CHECK_EQ(ev_.order_id, OrderId{taker_});                   CHECK_EQ(ev_.maker_id, OrderId{maker_});                   CHECK_EQ(ev_.price, Price{px_});                           CHECK_EQ(ev_.qty, Qty{qty_});                              CHECK_EQ(ev_.leaves, Qty{leaves_});                      } while (0)

#define CHECK_CANCELED(e, id_, px_, qty_, reason_)         do {                                                       const ::exsim::Event ev_ = (e);                          CHECK_EQ(ev_.type, ::exsim::EventType::Canceled);        CHECK_EQ(ev_.order_id, OrderId{id_});                    CHECK_EQ(ev_.price, Price{px_});                         CHECK_EQ(ev_.qty, Qty{qty_});                            CHECK(ev_.reason == (reason_));                        } while (0)

#define CHECK_REJECTED(e, id_, reason_)                    do {                                                       const ::exsim::Event ev_ = (e);                          CHECK_EQ(ev_.type, ::exsim::EventType::Rejected);        CHECK_EQ(ev_.order_id, OrderId{id_});                    CHECK(ev_.reason == (reason_));                        } while (0)
