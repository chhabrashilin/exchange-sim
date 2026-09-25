// Small statistics helpers for the benchmark harnesses.
#pragma once

#include <algorithm>
#include <array>
#include <vector>

#include "exsim/workload.hpp"

namespace exsim::bench {

inline double median_of(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

// Median of `v` with a 95% percentile-bootstrap confidence interval: {median, lo, hi}. Fixed seed, so the
// interval is reproducible for a given input.
inline std::array<double, 3> bootstrap_median_ci(const std::vector<double>& v, int iters = 5000) {
  if (v.size() < 3) return {median_of(v), median_of(v), median_of(v)};
  Rng rng(2024);
  std::vector<double> meds, draw(v.size());
  meds.reserve(static_cast<std::size_t>(iters));
  for (int i = 0; i < iters; ++i) {
    for (double& d : draw) d = v[rng.below(v.size())];
    meds.push_back(median_of(draw));
  }
  std::sort(meds.begin(), meds.end());
  return {median_of(v), meds[static_cast<std::size_t>(0.025 * iters)], meds[static_cast<std::size_t>(0.975 * iters)]};
}

// Elementwise a[i] / b[i] over paired repetitions (interleaved runs see the same machine state).
inline std::vector<double> paired_ratios(const std::vector<double>& a, const std::vector<double>& b) {
  std::vector<double> r;
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) r.push_back(a[i] / b[i]);
  return r;
}

}  // namespace exsim::bench
