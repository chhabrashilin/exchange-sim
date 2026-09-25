// Log-linear latency histogram (HdrHistogram-style), allocation-free on record.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "exsim/common.hpp"

namespace exsim {

// Each power-of-two range is split into 32 linear sub-buckets, so any recorded value is reported
// within 1/32 (~3%) relative error. Values below 32 are exact. record() is branch-light and
// allocation-free, so it can sit inside a hot loop. Values are unit-agnostic (the tools record TSC
// ticks and scale at report time).
class LatencyHistogram {
 public:
  static constexpr std::uint32_t kSubBits = 5;
  static constexpr std::uint32_t kSub = 1u << kSubBits;
  static constexpr std::uint32_t kBuckets = (64 - kSubBits + 1) * kSub;

  EXSIM_ALWAYS_INLINE void record(std::uint64_t v) noexcept {
    ++counts_[index(v)];
    ++count_;
    sum_ += v;
    min_ = std::min(min_, v);
    max_ = std::max(max_, v);
  }

  // Upper bound of the bucket holding the p-th percentile (0 < p <= 100), clamped to the max seen.
  std::uint64_t percentile(double p) const noexcept {
    if (count_ == 0) return 0;
    const auto rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(count_))));
    std::uint64_t cum = 0;
    for (std::uint32_t i = 0; i < kBuckets; ++i) {
      cum += counts_[i];
      if (cum >= rank) return std::min(upper_bound(i), max_);
    }
    return max_;
  }

  void merge(const LatencyHistogram& o) noexcept {
    for (std::uint32_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    count_ += o.count_;
    sum_ += o.sum_;
    min_ = std::min(min_, o.min_);
    max_ = std::max(max_, o.max_);
  }

  void reset() noexcept { *this = LatencyHistogram{}; }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

  static constexpr std::uint32_t index(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<std::uint32_t>(v);
    const auto msb = static_cast<std::uint32_t>(63 - std::countl_zero(v));
    const std::uint32_t shift = msb - kSubBits;
    return (shift + 1) * kSub + static_cast<std::uint32_t>(v >> shift) - kSub;
  }
  static constexpr std::uint64_t upper_bound(std::uint32_t idx) noexcept {
    if (idx < kSub) return idx;
    const std::uint32_t shift = idx / kSub - 1;
    const std::uint64_t sub = idx % kSub + kSub;
    return ((sub + 1) << shift) - 1;
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace exsim
