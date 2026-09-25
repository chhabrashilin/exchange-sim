# exchange-sim

A price-time priority matching engine, an order-entry gateway around it, and a market-making study run against **real
exchange data**, in C++20. The point of the project is not only that it is fast. Every claim below is measured, tested
against an independent oracle, or shown to be false and reported that way.

**Try it in a browser (no install): open [`ui/index.html`](ui/index.html).** It runs the *real* C++ engine compiled to
WebAssembly (62 KB), with a live order book you can trade against, and the study results as charts.

## What is demonstrated

| Claim | Evidence | Where |
|---|---|---|
| **Fast, allocation-free matching.** 18-26 M msg/s on one core (best of 15; the range is host noise between two campaigns), p50 52-106 ns / p99 564-879 ns per message, zero heap allocations on the hot path. | Two benchmark campaigns with bootstrap intervals; a test counts allocations. | [BENCHMARKS.md](BENCHMARKS.md) |
| **About 3x a textbook `std::map` engine (median of paired reps 2.6-3.1x, 95% CIs 1.9-3.9x) and 18-22x [Liquibook](https://github.com/enewhuis/liquibook) (CIs 15-25x).** Ratios were stable across campaigns while absolute throughput moved ~30%. | Same input, and the engines first agree on every trade (933,619 trades, 96,761,274 lots). | `exsim_bench`, `exsim_bench_liquibook` |
| **Correct.** | Differential testing against a naive reference (1.7 M events byte-identical), planted-bug mutation testing (4/4 caught), independent-engine agreement, ASan/UBSan/TSan. | [tests/](tests/) |
| **The real Binance book is reconstructed exactly.** 573/573 snapshots equal the exchange's own book (458,400 levels, 0 mismatches, 0 phantom trades) over 30 minutes of BTCUSDT; ETHUSDT likewise. | Compared at the exact sequence point; fault injection proves the check can fail. | [docs/DESIGN.md](docs/DESIGN.md#7-reconstructing-a-real-exchange-book) |
| **An acknowledged order is never lost.** | Real `kill -9` under load, recovery, digest equals an independent offline replay; torn journal tail detected. | `scripts/e2e_gateway.sh` |
| **A market-making study with an honest answer.** On real data, with queue-aware fills, latency and fees, passive quoting near the touch loses roughly 1.3-2 bps of traded notional on both BTC and ETH whichever strategy is used; Avellaneda-Stoikov's edge is inventory control, not spread capture. | Calibrated parameters, block-bootstrap intervals, an independent audit of the accounting. | [docs/RESEARCH.md](docs/RESEARCH.md) |

Measured on a 15 W laptop under WSL2, where a jitter probe shows the hypervisor taking 6-12% of a pinned core. Ratios within
a run are solid; absolute numbers move. The limits section says what is *not* established.

```
 Binance L2 feed        capture             mirror                 engine                strategy
+---------------+   +-------------+   +------------------+   +-----------------+   +-------------------+
| depth diffs   |   | .exmd, one  |   | levels -> orders |   | price-time      |   | Avellaneda-       |
| trades        |-->| local clock |-->| exact vs         |-->| priority, zero  |-->| Stoikov vs        |
| REST snapshots|   | int ticks   |   | exchange snapshot|   | allocation      |   | baselines,        |
+---------------+   +-------------+   +------------------+   +-----------------+   | queue-aware fills |
                                                                                    +-------------------+
 client --TCP--> gateway --> journal (write-ahead, CRC32C) --> risk gate --> engine --> exec reports
```

## Quick start

Requires CMake 3.20+, Ninja and GCC 13+ or Clang 17+ on Linux (the headers are portable; pinning, huge pages and the
gateway assume Linux/x86-64). On Windows, use WSL2; `run.ps1` drives a WSL build from PowerShell.

```bash
cmake --preset release && cmake --build --preset release
scripts/verify_all.sh                    # every correctness check, offline, ~1 minute

build/release/exsim_bench --spsc                           # design points + SPSC ring
build/release/exsim_mdreplay --in data/sample_btcusdt_30s.exmd   # real-data validation
build/release/exsim_mmsim --in data/sample_btcusdt_30s.exmd --strategy as --warmup-s 5
scripts/e2e_gateway.sh build/release                       # gateway, latency, crash recovery
```

The long captures behind the study are not committed (tens of MB); see [data/README.md](data/README.md) to capture your own.

## The engine, briefly

Header-only, no dependencies: [`include/exsim/`](include/exsim/). Full rationale and rejected alternatives in
[docs/DESIGN.md](docs/DESIGN.md).

- **Dense price ladder** (level = array index) with a **two-level bitmap** for next-best-price, a cached best price whose
  empty sentinels make "does it cross" one compare, and an **intrusive FIFO** per level so cancel is O(1).
- **Slab-allocated orders** with an embedded free list, prefaulted and huge-page aligned. Three interchangeable layouts
  (`AosStore`, `SoaStore`, `HybridStore`) behind one interface, chosen by measurement.
- **Flat id index**: open addressing, backward-shift deletion (no tombstones), 8-byte `{fingerprint, slot}` entries.
- **Order types:** limit (Day/IOC/FOK), market, post-only; cancel; modify (shrinking keeps priority, reprice or growth loses
  it and may trade); self-trade prevention; a hard capacity that cancels with `BookFull` instead of allocating.
- **Deterministic**: the output is a pure function of the ordered input, which is what makes journal replay, differential
  testing and the study reproducible.

## What the measurements changed

The first "optimized" engine was **slower** than `std::map` (0.74-0.85x). Instructions were down 60% and branch misses
84%, so the problem had to be memory: the id index was sized for capacity (8 MiB) while ~4,000 orders were live, and a
well-mixed hash scattered them across all of it. A locality-preserving hash gave **2.3x**, and 8-byte index entries another
11% off last-level misses. The structure-of-arrays layout that textbooks recommend *doubled* L1 misses and multiplied
last-level misses by 3.5; it lost, and stays in the suite as a measured negative result. The locality hash has a real
weakness (keys crafted against it slow lookups 300-500x, from ~1.5-3 ns to ~770-950 ns), quantified in `exsim_bench --adversarial`.

## Market data and the study

`scripts/capture_binance.py` records live depth diffs, trades and REST snapshots against one local clock;
`scripts/mdconv.py` converts to exact integer ticks and lots; `exsim_mdreplay` rebuilds the book in the engine and validates
it; `exsim_mmsim` runs strategies against it.

Two things went wrong before the validation passed, and both are documented because they are the interesting part:
applying a batch's additions before its removals makes the book transiently cross and the engine invents trades (109
phantom trades on a 30-second sample); and a diff-based reconstruction cannot know levels deeper than its seed snapshot that
never change, so the first 30-minute validation failed 147 of 573 snapshots until the comparison was restricted to the
provably complete region and the mirror reseeded as the market moved.

The study ([docs/RESEARCH.md](docs/RESEARCH.md)) estimates Avellaneda-Stoikov's `k` from real trades, models queue
position from observable flow, sweeps latency and fees, quantifies how much a naive "touch means filled" backtest overstates
results (about 7x the fills for touch quoting), and puts block-bootstrap intervals on everything.

## Verification

- **Differential testing** ([`test_differential.cpp`](tests/test_differential.cpp)): identical random streams to a naive
  `std::map` book and to every variant; events must be byte-identical after every command; full book compared every 50
  commands; edge-heavy generator (128-tick band, capacity 200, 4 owners, every order type, junk ids).
- **Mutation testing:** four bugs planted in the engine, all caught; a fifth, in the book mirror (additions before
  removals), is caught by a unit test *and* by real data (109 phantom trades).
- **Independent oracles:** Liquibook agrees on 3 M commands; the exchange's own snapshots agree on 30 minutes of BTCUSDT.
- **Zero-allocation hot path** is a test, not a claim (a global `operator new` counter).
- **Sanitizers:** ASan+UBSan on the full suite, TSan on the ring and the three-thread pipeline.
- **Durability:** write-ahead journal, CRC32C, torn-tail and bit-flip tests, real `kill -9` recovery.
- **Accounting audit:** `scripts/verify_accounting.py` recomputes every study run's PnL, inventory and spread capture from
  the raw fill logs with no shared code, and CI runs it on the committed results.
- **CI:** GCC and Clang with `-Werror`, ASan+UBSan, TSan, real-data validation with fault injection, gateway end to end,
  Liquibook agreement, WebAssembly build and test.

150 C++ test cases and 14 WebAssembly checks. The harness is a small self-registering framework, so the suite builds anywhere
the engine does.

## Bugs and wrong turns

| What happened | Caught by | Outcome |
|---|---|---|
| The optimized engine was slower than `std::map`. | Benchmark, then a capacity sweep isolating footprint | Locality-preserving index hash: 2.3x. |
| Structure-of-arrays doubled cache misses. | Deterministic cache simulation | AoS is the default; SoA and both splits are kept as negative results. |
| Real-data validation failed 147/573 snapshots. | Diagnosing the first mismatch by level | Not a matching bug: diff streams cannot reveal unchanged deep levels. Compare within the provable region, reseed. |
| Book batches applied additions first and invented 109 trades. | Mutation-testing the mirror | Removals-before-additions, pinned by a unit test and real data. |
| Exported CSV mids were quantized to a 10-tick grid. | The independent accounting audit | `setprecision(15)`; the simulator's internal numbers were always right. |
| My test macro sent each command three times. | A count that was 9, not 3 | Events evaluated once. |
| The rate limiter admitted a burst of 6 when set to 5. | A unit test | Off-by-one in the GCRA slack. |
| A test claimed table-stride keys collide under the locality hash. They do not; the fold defeats exactly that stride. | Writing the benchmark for it | Real collision keys built against the fold: 500x slowdown, measured. |
| The wire format dropped the ingress timestamp and modify owners, so a replay could not reproduce risk decisions. | Designing the recovery test | The journal record stores them; the CRC covers them. |
| Mutant M4 (stale FIFO tail) hung instead of failing. | Mutation testing | Cycle-safe invariant audit runs first; bounded walks; ctest timeout. |
| Cross-core latency of 7e18 ns. | A paced pipeline run | vCPU TSC skew made a delta negative; clamped and counted. |
| Clang's TSan runtime could not link the allocation counter. | Running the full CI matrix locally | Counter compiled out under TSan. |

## Repository layout

```
include/exsim/           the engine and infrastructure (header-only)
  order_book.hpp           matching            order_store.hpp    three layouts + slab
  order_index.hpp          id index, hashes    price_bitmap.hpp   next-best-price
  reference_book.hpp       naive oracle        matching_engine.hpp  routing
  protocol.hpp             wire + reports      journal.hpp        write-ahead log, CRC32C
  risk.hpp                 pre-trade risk      spsc_queue.hpp     lock-free ring
  md.hpp md_mirror.hpp     capture + book reconstruction
  mm.hpp mm_sim.hpp        queue model, strategies, simulation loop
tools/                   gen, replay, pipeline, server, client, journal, mdreplay, mmsim
bench/                   design-point suite, Liquibook head-to-head
tests/                   unit, differential, allocation, journal, risk, market-making
scripts/                 capture, convert, calibrate, experiments, analysis, audit, e2e, wasm build
wasm/                    C API + Node test for the WebAssembly build
ui/                      browser explorer (index.html + engine.js)
results/  data/  docs/   study outputs, sample capture, write-ups and figures
```

## Related work

[nanolob](https://github.com/Hellblazer704/nanolob) (a design-point benchmark journey, differential oracle, and real Binance
replay with an Avellaneda-Stoikov study; this project's method owes it a debt, and goes further on calibrating `k`,
latency and fee sweeps, fill-model optimism, a gateway with crash recovery, and a WebAssembly UI),
[Liquibook](https://github.com/enewhuis/liquibook) (the head-to-head baseline),
[CppTrader](https://github.com/chronoxor/CppTrader) (a mature engine and ITCH handler),
and the smaller engines [ohparekh/matching-engine](https://github.com/ohparekh/matching-engine),
[apurvapm/low-latency-cpp-LOB](https://github.com/apurvapm/low-latency-cpp-LOB) and
[makssuchecki/order-matching-engine](https://github.com/makssuchecki/order-matching-engine).

## Limits

- **No bare-metal numbers.** WSL2 hides the PMU and injects jitter; cache results are a simulator (no L2/TLB/prefetcher).
  Tails above ~p99 measure the hypervisor.
- **The study is ~1 hour of data across two assets.** One BTC session trended up. Intervals are wide and are reported.
- **Single matching thread per shard; no replication, no router.** The journal is the input a hot standby would need.
- **Client-chosen order ids** reach the locality hash. Exchanges assign ids; the gateway does not yet.
- **The fill model** assumes no own market impact, no hidden orders, and a neutral cancellation proration; book data is
  100 ms granular.
- **Durability** is tested against `kill -9`, not power loss or disk failure.
