// Event sinks: the template parameter that receives the engine's output.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include "exsim/common.hpp"

namespace exsim {

// Order-sensitive 64-bit digest of an event stream. Two runs produce equal digests iff (with
// overwhelming probability) they emitted the same events in the same order. This is how
// deterministic replay and cross-implementation equivalence are checked at scale.
class EventDigest {
 public:
  EXSIM_ALWAYS_INLINE void on_event(const Event& e) noexcept {
    std::uint64_t w[sizeof(Event) / 8];
    std::memcpy(w, &e, sizeof e);
    for (std::uint64_t x : w) h_ = std::rotl((h_ ^ x) * 0x9E3779B97F4A7C15ull, 27);
    ++count_;
  }
  std::uint64_t value() const noexcept {
    std::uint64_t k = h_ ^ count_;
    k ^= k >> 33, k *= 0xff51afd7ed558ccdull, k ^= k >> 33, k *= 0xc4ceb9fe1a85ec53ull, k ^= k >> 33;
    return k;
  }
  std::uint64_t count() const noexcept { return count_; }

 private:
  std::uint64_t h_ = 0xcbf29ce484222325ull;
  std::uint64_t count_ = 0;
};

// Digest plus per-type counters.
class CountingSink {
 public:
  EXSIM_ALWAYS_INLINE void on_event(const Event& e) noexcept {
    digest_.on_event(e);
    ++by_type_[static_cast<std::size_t>(e.type) & 7];
    if (e.type == EventType::Trade) volume_ += e.qty;
  }
  std::uint64_t digest() const noexcept { return digest_.value(); }
  std::uint64_t events() const noexcept { return digest_.count(); }
  std::uint64_t count(EventType t) const noexcept { return by_type_[static_cast<std::size_t>(t)]; }
  std::uint64_t volume() const noexcept { return volume_; }

 private:
  EventDigest digest_;
  std::array<std::uint64_t, 8> by_type_{};
  std::uint64_t volume_ = 0;
};

// Records every event (tests and tools only; allocates).
struct VectorSink {
  std::vector<Event> events;
  void on_event(const Event& e) { events.push_back(e); }
};

}  // namespace exsim
