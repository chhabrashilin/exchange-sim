#!/usr/bin/env bash
# Cache behaviour of each order-store layout.
#
# With a hardware PMU available (bare-metal Linux), this uses `perf stat` on the real counters.
# Otherwise (VMs, WSL2, CI runners) it falls back to Valgrind's cachegrind, which simulates the
# cache hierarchy. Those numbers are deterministic and comparable across layouts, but they are a
# model, not silicon. Only the timed matching loop is instrumented (cachegrind client requests), so
# workload generation and engine construction are excluded.
#
#   scripts/cache_profile.sh [build-dir] [messages]
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
N=${2:-1000000}
IMPLS=(ref aos-scatter aos soa hybrid)

if perf stat -e L1-dcache-load-misses true >/dev/null 2>&1; then
  for impl in "${IMPLS[@]}"; do
    echo "=== $impl (perf, hardware counters) ==="
    perf stat -e cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,branch-misses \
      "$BUILD/exsim_bench" --impl "$impl" --messages "$N" --reps 1 --no-latency 2>&1 | grep -E "cycles|instr|cache|LLC|branch"
  done
else
  echo "perf/PMU unavailable; using cachegrind (simulated caches)"
  for impl in "${IMPLS[@]}"; do
    echo "=== $impl (cachegrind) ==="
    valgrind --tool=cachegrind --instr-at-start=no --cache-sim=yes --branch-sim=yes --cachegrind-out-file=/dev/null \
      "$BUILD/exsim_bench" --impl "$impl" --messages "$N" --reps 1 --no-latency 2>&1 |
      grep -E "(I|D|LL|D1|LLd|Mispred|Branches).*(refs|misses|rate|Mispred|Branches)" | sed 's/^==[0-9]*== //'
  done
fi
