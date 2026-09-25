# exchange-sim

A price-time priority matching engine and exchange pipeline in C++20. It covers binary order entry,
lock-free SPSC rings between pinned threads, deterministic replay, and TSC-based latency
instrumentation. Every performance claim here is measured and every correctness claim is tested
against an independent oracle.

**Headline numbers:**
- **26.3 M msg/s** single-core matching on a mixed order-flow stream (best of 15; median 15.8 M), 2.7x a
  textbook `std::map` engine.
- **p50 52 ns / p99 564 ns** per message (new-order p50 37 ns).
- **Zero heap allocations** on the hot path, enforced by a test.
- **5.0 M msg/s** end-to-end through a three-thread feed → engine → market-data pipeline.

All of it runs on a 15 W laptop under WSL2, where a jitter probe shows the hypervisor stealing 12%
of a pinned core. See [BENCHMARKS.md](BENCHMARKS.md) for the full results, the methodology, and the
design choices that measurably *didn't* work.

```
  binary capture         SPSC ring           matching engine            SPSC ring        market data
┌───────────────┐    ┌──────────────┐    ┌──────────────────────┐    ┌──────────────┐   ┌──────────────┐
│ feed thread   │    │ 56 B Command │    │ engine thread        │    │ 48 B MdMsg   │   │ md thread    │
│ decode +     │───▶│ acquire/     │───▶│ price-time priority  │───▶│ trades +     │──▶│ consume +    │
│ validate     │    │ release,     │    │ per-symbol books     │    │ BBO updates  │   │ latency hist │
│ stamp TSC     │    │ cached index │    │ zero-alloc hot path  │    │              │   │              │
└───────────────┘    │ 42 M items/s │    └──────────┬───────────┘    └──────────────┘   └──────────────┘
      pinned         └──────────────┘               │ pinned                                  pinned
                                                    ▼
                                        event digest == single-threaded replay digest
```

- [What it does](#what-it-does)
- [Engine design](#engine-design)
- [Pipeline and instrumentation](#pipeline-and-instrumentation)
- [Verification](#verification)
- [Bugs and wrong turns](#bugs-and-wrong-turns)
- [Build and run](#build-and-run)
- [Repository layout](#repository-layout)
- [Related work](#related-work)
- [Limitations](#limitations)

## What it does

**Order types and semantics**

| | |
|---|---|
| Limit, Day | Match while crossing, then rest the remainder at the limit price. |
| Limit IOC | Match; cancel the remainder (`IocExpired`). |
| Limit FOK | All-or-nothing: pre-checks available liquidity within the limit (STP-aware), otherwise cancels with no fills. |
| Market | Sweep until filled or the book is empty; the remainder is canceled. |
| Post-only | Rejected (`WouldCross`) if it would take liquidity. |
| Cancel | O(1) unlink; empty price levels are retired automatically. |
| Modify | Reducing size at the same price amends in place and **keeps priority**. A reprice or size increase is cancel/replace: it **loses priority** and may trade immediately. |
| Self-trade prevention | `CancelResting`, `CancelIncoming`, or `None` (for faithful replay). |
| Rejects | Unknown symbol, id 0, zero qty, price outside the band, duplicate live id, unknown order. Dead ids may be reused, as on real venues. |
| Capacity | Fixed per book. If an order's remainder cannot rest, it is canceled with `BookFull`. The engine never allocates. |

Prices are integer ticks and quantities integer lots. There is no floating point on the matching
path. Every outbound `Event` is a 40-byte record with no implicit padding
(`static_assert(has_unique_object_representations_v<Event>)`), so event streams compare and hash
byte-for-byte.

## Engine design

Header-only, no dependencies: [`include/exsim/`](include/exsim/).

| Concern | Choice | Why not the obvious thing |
|---|---|---|
| Price levels | Dense ladder indexed by `price − min_price`; 16-byte levels `{agg qty, head, tail}` | A `std::map` of price levels is a pointer-chasing tree walk plus an allocation per new level. An array index is one address computation. |
| Best price | Cached index; empty sides are `-1` / `num_levels`, so "does it cross?" is one signed compare with no branch on emptiness | |
| Next best price | Two-level `PriceBitmap` (`countr_zero`/`countl_zero` over a summary word) | Walking empty levels is O(gap); this is at most 2 words plus a 16-word summary scan for a 65,536-tick band. |
| FIFO per level | Intrusive doubly linked list of 32-bit slot indices. The head's `prev` is never read, so popping the head writes one field. | `std::list` allocates a node per order. |
| Order storage | Slab of preallocated slots with an embedded LIFO free list, prefaulted and backed by 2 MiB-aligned transparent huge pages | A LIFO free list reuses the most recently freed (cache-warm) slot. |
| Order layout | Compile-time policy with three interchangeable layouts: `AosStore` (48 B record), `SoaStore`, `HybridStore` (32 B hot + cold audit) | Chosen by measurement. The default is AoS; SoA doubled cache misses ([why](BENCHMARKS.md#2--3--4-the-structure-of-arrays-hypothesis-refuted)). |
| Id → order | Open addressing, linear probing, backward-shift deletion, 8-byte `{fingerprint, slot}` entries verified against the order record | No tombstones, so a cancel-heavy flow cannot degrade probe lengths over time. |
| Index hash | **Locality-preserving** fold, not fmix64 | The single biggest win: **2.3x** ([why](BENCHMARKS.md#1--2-the-finding-that-mattered-most)). |
| Event dispatch | `Sink` template parameter | No virtual call per event. A sink that ignores events costs nothing. |
| Symbols | Independent books; ids unique per symbol | Books share nothing, so symbols can be sharded across cores. |

## Pipeline and instrumentation

- **Wire protocol** ([`protocol.hpp`](include/exsim/protocol.hpp)). Fixed-size, packed, little-endian
  messages with a 16-byte header (length, type, version, symbol, sequence), in the spirit of OUCH/ITCH.
  Decoding copies with `memcpy` into local structs, which is alignment-safe with no aliasing UB.
  Every enum and length is validated. Malformed messages are skipped by length, and a corrupt
  length field stops decoding rather than guessing at framing.
- **SPSC ring** ([`spsc_queue.hpp`](include/exsim/spsc_queue.hpp)):
  - power-of-two capacity with free-running indices
  - acquire/release only
  - producer and consumer lines kept 128 B apart (the adjacent-line prefetcher pulls lines in pairs)
  - each side caches the other's index
  - consumer-side prefetch of already-published slots, measured at +11–15% pipeline throughput
- **CPU pinning** via `pthread_setaffinity_np`, plus thread names.
- **Latency** ([`clock.hpp`](include/exsim/clock.hpp), [`latency_histogram.hpp`](include/exsim/latency_histogram.hpp)):
  - `lfence; rdtsc … rdtscp; lfence` brackets, calibrated against `steady_clock`
  - an allocation-free log-linear histogram with ≤ 1/32 relative error
  - paced runs stamp the **scheduled** send time, which corrects for coordinated omission
  - cross-core deltas are clamped and counted, because vCPU TSCs can disagree (see bugs)
- **Deterministic replay** ([`exsim_replay`](tools/exsim_replay.cpp)). A binary capture replays to an
  order-sensitive 64-bit event digest, optionally writing a raw event journal. The same capture
  gives the same digest on every run, under every book implementation, and through the threaded
  pipeline.
- **Hiccup meter**. `exsim_pipeline --jitter N` measures how much time the OS/hypervisor steals
  from a pinned core. It is the noise floor for every latency figure.

## Verification

**Test suite:** 129 test cases, 2.4 M assertions, and a zero-dependency harness, so it builds
anywhere the engine does.

- **Randomized differential testing** ([`test_differential.cpp`](tests/test_differential.cpp)).
  Identical random streams go to the naive `ReferenceBook` (`std::map`/`std::list`) and to all four
  optimized variants. After *every* command the emitted events must be byte-identical. Every 50
  commands the full resting book is compared order-by-order, and each implementation's structural
  invariants are audited: link consistency, level aggregates, bitmap/best-price agreement, and
  index ↔ store agreement. The stream is built to hit edge cases:
  - a 128-tick band, so levels constantly empty and refill and orders hit the band edges
  - capacity 200, so `BookFull` fires
  - 4 owners, so STP fires constantly
  - every order type, plus cancels/modifies of live, dead and never-seen ids, duplicate ids, and
    invalid fields

  Coverage: 120 seeds × 10k commands × all 3 STP policies, 1.7 M events compared.
- **The oracle is itself tested.** `ReferenceBook` runs the same 23 hand-written semantic scenarios
  as the fast books (`BOOK_TEST` registers each scenario for all five implementations). It is also
  benchmark design point 0, so it cannot silently rot.
- **Mutation testing.** Four bugs were planted by hand in `order_book.hpp`, one at a time, and every
  one was caught:
  1. in-place modify on a size increase: caught by a snapshot divergence at op 9,350
  2. a missing bitmap clear: caught by an invariant violation at op 50
  3. an FOK check that ignores STP: caught by an event divergence at op 4,467
  4. a stale tail pointer: caught by the unit tests and invariant audits
- **Zero-allocation hot path** ([`test_alloc.cpp`](tests/test_alloc.cpp)). A global `operator new`
  counter must read 0 across 300k messages for every optimized book. As a sanity check, it reads
  more than 10k for the reference book.
- **Determinism.** The workload generator is a pure function of its seed. Encode → decode → replay
  reproduces the digest.
- **Sanitizers.** The full suite passes under ASan + UBSan (GCC and Clang) and TSan (Clang). The SPSC tests and the full
  three-thread pipeline are clean under ThreadSanitizer, and the TSan pipeline digest matches
  replay.
- **CI** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): GCC and Clang with `-Werror`,
  ASan+UBSan, and TSan. It also checks cross-implementation replay-digest equality and
  pipeline == replay.

## Bugs and wrong turns

Problems the tests and profiling turned up, and how each was resolved.

| What happened | Caught by | Outcome |
|---|---|---|
| **The optimized engine was slower than `std::map`.** The first full benchmark put every "optimized" book at 0.74–0.85x the textbook baseline. | Benchmark, then a capacity sweep that isolated footprint | The 8 MiB id index, sized for capacity and scattered by fmix64, cost a cache/TLB miss per message. The locality-preserving hash gave 2.3x. |
| **The SoA premise was wrong.** The layout expected to cut cache misses doubled them. | cachegrind (deterministic) after wall-clock proved too noisy | AoS is the default. SoA and the hot/cold split are kept as measured negative results. |
| **A first hot/cold split (16 B + 16 B) raised LL misses 59%** | cachegrind | Replaced by the 32 B hot record (+19% LL, still behind AoS). Both results are documented. |
| **Benchmark noise hid everything.** Identical configurations gave medians from 3.3 to 8.8 M msg/s. | Rerunning unchanged configs | Reps are interleaved round-robin across implementations, best-of-N is the headline, and the jitter probe quantifies the floor. |
| **Dangling reference in the test macro.** `CHECK_EQ` bound `const auto&` to `optional.value()` of a temporary. | GCC `-Wdangling-reference` on the first build | The macro now evaluates by value through an out-of-line helper, which also cut `test_book.cpp` compile time from 3+ minutes to seconds. |
| **Mutant M4 hung instead of failing.** A stale tail pointer made the FIFO cyclic and the snapshot walk looped forever. | Mutation testing | Invariant audits (cycle-safe) now run before snapshots, snapshot walks are bounded, and ctest has a timeout. |
| **Cross-core latency of 7×10^18 ns** | Paced pipeline run | vCPU TSC skew under Hyper-V made a delta negative, and unsigned subtraction wrapped. Negative deltas are now clamped and counted. |
| **Distro compiler flags leaking into benchmarks.** conda's activation injects `-fPIC -fno-plt -fstack-protector-strong -march=nocona -O2`. | Reading the actual compile line | The build overrides `CXXFLAGS`. |
| **The Clang TSan CI job could never have linked.** Clang's TSan runtime statically defines global `operator new`/`delete`, which clashed with the allocation-counting replacements. GCC's shared-runtime TSan had linked fine. | Running the CI matrix locally with Clang 23 before pushing | The replacements are compiled out under TSan, and the allocation test reports itself skipped there (ASan/UBSan and `-Werror` jobs still enforce it). |
| **A failed rebuild silently benchmarked the old binary** | Output missing a line the new code prints | The build script now fails loudly. The sweep was rerun. |

## Build and run

Requires CMake ≥ 3.20, Ninja, and GCC ≥ 13 or Clang ≥ 17 on Linux (the engine headers are
portable; pinning, huge pages and the benchmarks assume Linux/x86-64).

```bash
cmake --preset release && cmake --build --preset release
ctest --preset release                                 # 129 cases

./build/release/exsim_bench --spsc                     # all design points + SPSC ring
./build/release/exsim_gen --out flow.bin --messages 10000000
./build/release/exsim_replay --in flow.bin --book aos  # prints the event digest
./build/release/exsim_pipeline --in flow.bin           # saturated throughput
./build/release/exsim_pipeline --in flow.bin --rate 1000000   # latency under load
./build/release/exsim_pipeline --jitter 10             # machine noise floor

cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
scripts/cache_profile.sh build/release                 # perf counters, or cachegrind fallback
```

## Explorer UI

`ui/index.html` is a self-contained page (no build step, no dependencies). Open it in a browser:

- **Live order book.** Send limit, market, IOC, FOK and post-only orders, watch the depth ladder and trade tape
  update, cancel your resting orders, and turn on a random market simulator.
- **Benchmark results.** Charts of throughput, latency and cache misses for every design point.
- **How it works.** A one-screen summary of the rules.

The page runs a small JavaScript port of the matching rules (a browser timing button is clearly labeled as
*not* the C++ number). It can be hosted as-is with GitHub Pages.

## Repository layout

```
include/exsim/            the engine and pipeline primitives: header-only, no dependencies
  common.hpp                ticks/lots types, Command (56 B), Event (40 B), BookConfig
  order_book.hpp            matching: ladder, bitmap, FIFO, STP, FOK, modify
  order_store.hpp           slab pool + three layouts (AoS / SoA / hybrid)
  order_index.hpp           flat id index, scatter vs locality hash
  price_bitmap.hpp          two-level occupancy bitset
  reference_book.hpp        naive oracle + design point 0
  matching_engine.hpp       multi-symbol routing, book aliases
  protocol.hpp              binary wire format + validating decoder
  spsc_queue.hpp            lock-free ring
  latency_histogram.hpp     log-linear histogram
  clock.hpp  cpu.hpp        TSC timing; pinning
  memory.hpp                prefaulted, huge-page-aligned arrays
  sinks.hpp                 event digest / counters
  workload.hpp  wire_file.hpp   deterministic flow generator; capture I/O
tools/                    exsim_gen, exsim_replay, exsim_pipeline
bench/                    exsim_bench (design points, latency, SPSC)
tests/                    unit, differential, allocation, determinism
scripts/                  bench.sh, cache_profile.sh
ui/index.html             browser explorer: live book + benchmark charts
run.ps1                   Windows launcher that drives the WSL build
BENCHMARKS.md             methodology, results, negative results
```

## Related work

Projects I looked at while building this. Each takes a different approach to the same
problem:

- [Hellblazer704/nanolob](https://github.com/Hellblazer704/nanolob): the most direct inspiration
  for the *engineering method*: a step-by-step benchmark of design points, a negative result reported as measured, a
  differential oracle that doubles as the baseline, and a bugs-found log. It goes further on market
  data (real Binance L2 replay validated against exchange snapshots, Avellaneda–Stoikov
  market-making). exsim instead goes further on layout experiments, the binary wire/pipeline side,
  and index locality.
- [chronoxor/CppTrader](https://github.com/chronoxor/CppTrader): a mature matching engine and
  ITCH handler.
- [enewhuis/liquibook](https://github.com/enewhuis/liquibook): a long-lived header-only matching
  engine.
- Smaller single-engine projects with similar goals:
  [ohparekh/matching-engine](https://github.com/ohparekh/matching-engine) (lock-free ingress),
  [apurvapm/low-latency-cpp-LOB](https://github.com/apurvapm/low-latency-cpp-LOB) (indexed ladder),
  [makssuchecki/order-matching-engine](https://github.com/makssuchecki/order-matching-engine).

## Limitations

Known limits:

- **Single matching thread per shard.** Symbols are independent, so sharding is straightforward,
  but no multi-shard router is implemented.
- **No persistence or recovery.** The event journal is replayable output, not a recovery log, and
  there is no sequencer failover.
- **Client-chosen order ids** with the locality hash are a latency-DoS vector if clients are
  untrusted (see BENCHMARKS.md). A production gateway would assign ids.
- **Fixed price band per book.** Orders outside the band are rejected, as with exchange price
  collars; the band does not re-center.
- **Measured on a noisy laptop under WSL2.** Ratios within a run are solid. Absolute numbers move,
  and tails above ~p99 measure the hypervisor. No `isolcpus`/`nohz_full` results yet.
- **No hardware-counter cache numbers.** The PMU is not exposed under WSL2, so cache results are
  cachegrind simulations (no L2/TLB/prefetcher). `scripts/cache_profile.sh` switches to `perf stat`
  on bare metal.
