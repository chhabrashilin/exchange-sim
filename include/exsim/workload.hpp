// Deterministic synthetic order flow.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "exsim/common.hpp"
#include "exsim/matching_engine.hpp"

namespace exsim {

// xoshiro256** seeded through splitmix64: fast, high quality, and bit-reproducible across platforms.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept {
    for (auto& w : s_) {
      seed += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      w = z ^ (z >> 31);
    }
  }
  std::uint64_t next() noexcept {
    const std::uint64_t r = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0], s_[3] ^= s_[1], s_[1] ^= s_[2], s_[0] ^= s_[3], s_[2] ^= t, s_[3] = rotl(s_[3], 45);
    return r;
  }
  // Uniform in [0, n) for n < 2^32 (Lemire's multiply-shift on 32 random bits).
  std::uint64_t below(std::uint64_t n) noexcept {
    return ((next() >> 32) * n) >> 32;  // n < 2^32
  }
  double uniform() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  bool chance(double p) noexcept { return uniform() < p; }
  // Geometric on {0, 1, 2, ...} with success probability p (mean (1-p)/p).
  std::uint32_t geometric(double p) noexcept {
    const double u = 1.0 - uniform();  // (0, 1]
    return static_cast<std::uint32_t>(std::floor(std::log(u) / std::log1p(-p)));
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
  std::uint64_t s_[4];
};

struct WorkloadConfig {
  std::uint64_t messages = 10'000'000;
  std::uint32_t symbols = 8;
  std::uint64_t seed = 42;
  std::uint32_t target_live = 4096;  // steady-state resting orders per symbol
  // Restrict to semantics every engine shares (Day/IOC limit orders and cancels): no modifies, FOK,
  // post-only or true market orders (market orders become IOC limits priced through the book).
  bool simple = false;
  BookConfig book{};
};

struct WorkloadMix {
  std::uint64_t new_orders = 0, cancels = 0, modifies = 0, marketable = 0, ioc_fok_market = 0;
};

// Generates a message stream shaped like real equity order flow. It is cancel-heavy (~40% of
// messages), resting interest sits a few ticks from a random-walking mid, ~10% of new orders cross
// the spread, and there are IOC/FOK/market/post-only orders, reprices, and in-place size reductions.
// A small fraction of cancels target orders that have already filled, as when a cancel races a fill
// on a real venue.
//
// The generator runs the stream through a real engine as it goes, so cancels and modifies
// overwhelmingly target orders that are actually live. The result is a pure function of the config.
inline std::vector<Command> generate_workload(const WorkloadConfig& cfg, WorkloadMix* mix_out = nullptr) {
  struct SymState {
    Price mid;
    std::vector<OrderId> live;
    std::unordered_map<OrderId, std::size_t> pos;
    void add(OrderId id) {
      if (pos.emplace(id, live.size()).second) live.push_back(id);
    }
    void remove(OrderId id) {
      auto it = pos.find(id);
      if (it == pos.end()) return;
      const std::size_t i = it->second;
      pos.erase(it);
      if (i + 1 != live.size()) {
        live[i] = live.back();
        pos[live[i]] = i;
      }
      live.pop_back();
    }
  };
  struct Tracker {
    std::vector<OrderId> filled;
    void on_event(const Event& e) {
      if (e.type == EventType::Trade && e.leaves == 0) filled.push_back(e.maker_id);
      if (e.type == EventType::Canceled && e.reason == Reason::SelfTrade) filled.push_back(e.order_id);
    }
  };

  Rng rng(cfg.seed);
  MatchingEngine<DefaultBook> engine(cfg.symbols, cfg.book);
  const Price lo = cfg.book.min_price + 512;
  const Price hi = cfg.book.min_price + static_cast<Price>(cfg.book.num_levels) - 512;
  std::vector<SymState> st(cfg.symbols);
  for (auto& s : st) s.mid = cfg.book.min_price + static_cast<Price>(cfg.book.num_levels / 2);

  std::vector<Command> out;
  out.reserve(cfg.messages);
  WorkloadMix mix;
  Tracker tracker;
  OrderId next_id = 1;

  for (std::uint64_t i = 0; i < cfg.messages; ++i) {
    const auto sym = static_cast<SymbolId>(rng.below(cfg.symbols));
    SymState& s = st[sym];
    if (rng.chance(0.02)) s.mid = std::clamp<Price>(s.mid + (rng.chance(0.5) ? 1 : -1), lo, hi);

    Command c{};
    c.symbol = sym;
    c.seq = i + 1;
    const double fill = static_cast<double>(s.live.size()) / cfg.target_live;
    const double p_cancel = std::min(0.40 * fill, 0.75);
    const double r = rng.uniform();

    if (!s.live.empty() && r < p_cancel) {
      c.type = MsgType::Cancel;
      c.order_id = rng.chance(0.01) ? 1 + rng.below(next_id) : s.live[rng.below(s.live.size())];
      ++mix.cancels;
    } else if (!cfg.simple && !s.live.empty() && r < p_cancel + 0.05) {
      c.type = MsgType::Modify;
      c.order_id = s.live[rng.below(s.live.size())];
      const auto o = engine.book(sym).find_order(c.order_id);
      if (!o) std::abort();  // tracker out of sync with the book: a generator bug
      if (rng.chance(0.5)) {  // reduce size in place: keeps priority
        c.price = o->price;
        c.qty = std::max<Qty>(1, o->qty / 2);
      } else {  // reprice by a few ticks: loses priority, may cross
        const auto d = static_cast<Price>(1 + rng.below(3));
        c.price = std::clamp<Price>(rng.chance(0.5) ? o->price + d : o->price - d, lo, hi);
        c.qty = o->qty;
      }
      ++mix.modifies;
    } else {
      c.type = MsgType::NewOrder;
      c.order_id = next_id++;
      c.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
      c.owner = static_cast<OwnerId>(1 + rng.below(64));
      c.qty = rng.chance(0.1) ? static_cast<Qty>(1 + rng.below(99)) : 100u * (1u + std::min(rng.geometric(0.45), 49u));
      const double t = rng.uniform();
      if (t < 0.02 && !cfg.simple) {
        c.ord_type = OrdType::Market;
        ++mix.ioc_fok_market;
      } else {
        c.ord_type = OrdType::Limit;
        const bool buy = c.side == Side::Buy;
        if (rng.chance(0.10)) {  // marketable: cross by a few ticks
          const auto d = static_cast<Price>(rng.geometric(0.5));
          c.price = buy ? s.mid + d : s.mid - d;
          ++mix.marketable;
        } else {  // passive: sit behind the touch
          const auto d = static_cast<Price>(1 + std::min(rng.geometric(0.15), 400u));
          c.price = buy ? s.mid - d : s.mid + d;
        }
        if (t < 0.05) {
          c.tif = Tif::Ioc;
          ++mix.ioc_fok_market;
          if (cfg.simple && t < 0.02) c.price = buy ? hi : lo;  // the 'market' slice, as an IOC through the book
        } else if (!cfg.simple && t < 0.06) {
          c.tif = Tif::Fok;
          ++mix.ioc_fok_market;
        } else if (!cfg.simple && t < 0.09) {
          c.flags = kFlagPostOnly;
        }
      }
      ++mix.new_orders;
    }

    out.push_back(c);
    tracker.filled.clear();
    engine.process(c, tracker);
    for (OrderId id : tracker.filled) s.remove(id);
    if (c.type == MsgType::Cancel) {
      s.remove(c.order_id);
    } else if (engine.book(sym).contains(c.order_id)) {
      s.add(c.order_id);
    } else {
      s.remove(c.order_id);
    }
  }
  if (mix_out != nullptr) *mix_out = mix;
  return out;
}

}  // namespace exsim
