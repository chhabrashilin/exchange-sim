#!/usr/bin/env bash
# Builds Release and runs the full benchmark suite: design points, SPSC ring, and the threaded pipeline.
#
#   scripts/bench.sh [build-dir]
#
# For stable numbers on a dedicated Linux box, also consider: performance governor
# (cpupower frequency-set -g performance), disabling turbo, isolcpus/nohz_full for the pinned
# cores, and moving IRQs off them. None of that was done for the numbers in BENCHMARKS.md.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD"

"$BUILD/exsim_bench" --messages 5000000 --reps 15 --spsc
"$BUILD/exsim_gen" --out "$BUILD/flow.bin" --messages 10000000
"$BUILD/exsim_replay" --in "$BUILD/flow.bin"
echo "--- pipeline, saturated (throughput) ---"
"$BUILD/exsim_pipeline" --in "$BUILD/flow.bin"
echo "--- pipeline, paced at 1M msg/s (latency under load) ---"
"$BUILD/exsim_pipeline" --in "$BUILD/flow.bin" --rate 1000000
echo "--- noise floor: how much does the OS/hypervisor steal from a pinned core? ---"
"$BUILD/exsim_pipeline" --jitter 10
