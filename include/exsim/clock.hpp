// Cycle-accurate timestamps for latency instrumentation.
#pragma once

#include <chrono>
#include <cstdint>

#include "exsim/common.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define EXSIM_HAS_TSC 1
#else
#define EXSIM_HAS_TSC 0
#endif

namespace exsim {

// Unserialized TSC read: cheapest, used for pipeline stamps where reordering by a few cycles is noise.
EXSIM_ALWAYS_INLINE std::uint64_t rdtsc() noexcept {
#if EXSIM_HAS_TSC
  return __rdtsc();
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Bracketing pair for timing a short region. The lfences keep the measured instructions from moving
// across the stamps (Intel's recommended pattern for rdtsc-based measurement).
EXSIM_ALWAYS_INLINE std::uint64_t tsc_begin() noexcept {
#if EXSIM_HAS_TSC
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return rdtsc();
#endif
}

EXSIM_ALWAYS_INLINE std::uint64_t tsc_end() noexcept {
#if EXSIM_HAS_TSC
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return rdtsc();
#endif
}

EXSIM_ALWAYS_INLINE void cpu_relax() noexcept {
#if EXSIM_HAS_TSC
  _mm_pause();
#endif
}

// Nanoseconds per TSC tick, measured against steady_clock over a busy-wait window. The TSC is
// invariant on every x86 part built in the last fifteen years, so one calibration holds for the run.
inline double calibrate_ns_per_tick(std::chrono::milliseconds window = std::chrono::milliseconds(100)) {
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t c0 = tsc_begin();
  while (clk::now() - t0 < window) {
  }
  const std::uint64_t c1 = tsc_end();
  const auto t1 = clk::now();
  const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  return ns / static_cast<double>(c1 - c0);
}

}  // namespace exsim
