// Two-level occupancy bitset over the price ladder.
#pragma once

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "exsim/common.hpp"

namespace exsim {

// Level 0 holds one bit per price level and level 1 one bit per non-zero level-0 word. When the best
// level empties, the next best is found by inspecting at most two words plus a scan of the summary
// (num_levels / 4096 words: 16 for a 65,536-tick band). The matcher never walks empty price levels.
class PriceBitmap {
 public:
  explicit PriceBitmap(std::uint32_t n) : n_(static_cast<std::int32_t>(n)), words_(n / 64), summary_((n / 64 + 63) / 64) {
    if (n == 0 || n % 64 != 0 || n > (1u << 30))
      throw std::invalid_argument("PriceBitmap: size must be a positive multiple of 64, at most 2^30");
  }

  EXSIM_ALWAYS_INLINE void set(std::int32_t i) noexcept {
    const auto u = static_cast<std::uint32_t>(i);
    words_[u >> 6] |= bit(u);
    summary_[u >> 12] |= bit(u >> 6);
  }

  EXSIM_ALWAYS_INLINE void clear(std::int32_t i) noexcept {
    const auto u = static_cast<std::uint32_t>(i);
    const std::uint32_t w = u >> 6;
    words_[w] &= ~bit(u);
    if (words_[w] == 0) summary_[w >> 6] &= ~bit(w);
  }

  bool test(std::int32_t i) const noexcept {
    const auto u = static_cast<std::uint32_t>(i);
    return (words_[u >> 6] & bit(u)) != 0;
  }

  // Smallest set index >= i, or size() if there is none.
  EXSIM_ALWAYS_INLINE std::int32_t find_next(std::int32_t i) const noexcept {
    if (i >= n_) return n_;
    if (i < 0) i = 0;
    const auto u = static_cast<std::uint32_t>(i);
    std::uint32_t w = u >> 6;
    const std::uint64_t m = words_[w] & (~0ull << (u & 63));
    if (m != 0) return static_cast<std::int32_t>((w << 6) | static_cast<std::uint32_t>(std::countr_zero(m)));
    if (++w >= words_.size()) return n_;
    std::size_t s = w >> 6;
    std::uint64_t sm = summary_[s] & (~0ull << (w & 63));
    while (sm == 0) {
      if (++s >= summary_.size()) return n_;
      sm = summary_[s];
    }
    w = static_cast<std::uint32_t>((s << 6) | static_cast<std::size_t>(std::countr_zero(sm)));
    return static_cast<std::int32_t>((w << 6) | static_cast<std::uint32_t>(std::countr_zero(words_[w])));
  }

  // Largest set index <= i, or -1 if there is none.
  EXSIM_ALWAYS_INLINE std::int32_t find_prev(std::int32_t i) const noexcept {
    if (i < 0) return -1;
    if (i >= n_) i = n_ - 1;
    const auto u = static_cast<std::uint32_t>(i);
    std::uint32_t w = u >> 6;
    const std::uint64_t m = words_[w] & (~0ull >> (63 - (u & 63)));
    if (m != 0) return static_cast<std::int32_t>((w << 6) | (63u - static_cast<std::uint32_t>(std::countl_zero(m))));
    if (w == 0) return -1;
    --w;
    auto s = static_cast<std::int64_t>(w >> 6);
    std::uint64_t sm = summary_[static_cast<std::size_t>(s)] & (~0ull >> (63 - (w & 63)));
    while (sm == 0) {
      if (--s < 0) return -1;
      sm = summary_[static_cast<std::size_t>(s)];
    }
    w = static_cast<std::uint32_t>(s << 6) | (63u - static_cast<std::uint32_t>(std::countl_zero(sm)));
    return static_cast<std::int32_t>((w << 6) | (63u - static_cast<std::uint32_t>(std::countl_zero(words_[w]))));
  }

  std::int32_t size() const noexcept { return n_; }

 private:
  static constexpr std::uint64_t bit(std::uint32_t i) noexcept { return 1ull << (i & 63); }

  std::int32_t n_;
  std::vector<std::uint64_t> words_;
  std::vector<std::uint64_t> summary_;
};

}  // namespace exsim
