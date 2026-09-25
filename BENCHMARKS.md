# Benchmarks

Every number here comes from the code in this repository and can be regenerated with
`scripts/bench.sh` and `scripts/cache_profile.sh`. Where a design choice did not pay off, that is
reported as measured, not dropped.

## Environment and its limits

| | |
|---|---|
| CPU | Intel Core i7-1355U (2P + 8E cores, 15 W laptop part), 48 KiB L1d / 1.25 MiB L2 per P-core, 12 MiB L3 |
| OS | Ubuntu 26.04 under **WSL2** (Hyper-V VM) on Windows 11, kernel 6.6 |
| Compiler | GCC 16.2, `-O3 -march=native`, C++20 |
| Tuning | Threads pinned with `pthread_setaffinity_np`. **No** isolcpus, no fixed clocks, turbo on, on battery/AC as found. |

This machine is a hostile benchmarking environment, and that is measured, not assumed.
`exsim_pipeline --jitter 10` spins a pinned thread on back-to-back `rdtsc` reads:

```
jitter probe: 10.0 s on cpu 4 (pinned: yes)
  TSC read-to-read gap  n=755103595  p50 10  p90 17  p99 31  p99.9 48  p99.99 220  max 10833984 ns
  time lost to gaps > 1 us: 12.250%; gaps > 10 us: 14533 (1453.3 per second)
```

The hypervisor takes the core away about 1,450 times a second, for up to 10.8 ms at a time. This
has two consequences:

1. **Wall-clock throughput drifts by up to 2x between runs** (thermal and turbo budget, host load).
   Repetitions are therefore interleaved round-robin across implementations, and best-of-N is the
   headline figure (noise only ever adds time), with the median alongside. Ratios measured within
   one run are trustworthy; absolute numbers move.
2. **Tail latency above roughly p99 measures the hypervisor, not the code.** Per-message engine
   percentiles are reported up to p99.9; pipeline tails are reported but attributed.

For cache behaviour, wall-clock is too noisy to settle small layout questions, so the deterministic
instrument is **cachegrind**: simulated 48 KiB D1 / 12 MiB LL. It has no L2, no TLB and no hardware
prefetcher, and only the timed matching loop is instrumented, via client requests. Its counts are
exact and repeatable, but they are a model, not silicon. On bare-metal Linux,
`scripts/cache_profile.sh` uses `perf stat` hardware counters instead.

## Workload

`generate_workload()` produces a deterministic stream (seed 42) across 8 symbols:
63.6% new orders (6.2% marketable, 3.8% IOC/FOK/market, ~3% post-only), 31.4% cancels (1% of them
for orders that already filled, as when a cancel races a fill), and 5.0% modifies (half in-place
size reductions, half reprices). Resting interest sits a geometric distance from a random-walking
mid, and each book holds about 4,096 live orders in steady state. The 5M-message benchmark stream
produces 1.46M trades.

Each book is configured for a 65,536-tick price band and **262,144 order capacity**, sized for peak
load rather than the steady state. That gap is what made design point 2 matter.

## Results by design point

Every design point below is still compiled into `exsim_bench` and runs against the identical stream.
All five produce **byte-identical event streams** (digest `b5661fe697165fae`), checked on every
benchmark run.

**Throughput**: 5M messages, 15 interleaved reps, pinned to one core:

| # | Design point | M msg/s best | M msg/s median | ns/msg (best) | vs baseline | heap allocs in loop |
|---|---|---:|---:|---:|---:|---:|
| 0 | `ref`: `std::map<price, std::list<Order>>` + `std::unordered_map` | 9.68 | 7.61 | 103.3 | 1.00x | 4,934,143 |
| 1 | `aos-scatter`: dense ladder + bitmap + slab pool + flat index (fmix64) | 11.41 | 8.52 | 87.6 | 1.18x | 0 |
| 2 | `aos`: same + **locality-preserving index hash** (shipped default) | **26.31** | **15.75** | **38.0** | **2.72x** | 0 |
| 3 | `soa`: same, pure structure-of-arrays order store | 24.11 | 16.86 | 41.5 | 2.49x | 0 |
| 4 | `hybrid`: same, 32 B hot record + cold audit array | 25.17 | 17.83 | 39.7 | 2.60x | 0 |

**Per-message latency** (serialized `lfence; rdtsc … rdtscp; lfence` around `engine.process`; the
timer's own ~17 ns is not subtracted; 5 interleaved passes):

| Design point | p50 | p90 | p99 | p99.9 | mean |
|---|---:|---:|---:|---:|---:|
| `ref` | 119 ns | 300 ns | 1,030 ns | 3,923 ns | 190 ns |
| `aos-scatter` | 177 ns | 380 ns | 760 ns | 3,040 ns | 235 ns |
| **`aos`** | **52 ns** | **184 ns** | **564 ns** | **2,305 ns** | **103 ns** |
| `soa` | 53 ns | 174 ns | 478 ns | 2,010 ns | 98 ns |
| `hybrid` | 50 ns | 187 ns | 588 ns | 1,961 ns | 106 ns |

By message type (`aos`): new order p50 **37 ns** / p99 429 ns; cancel p50 125 ns / p99 711 ns;
modify p50 125 ns / p99 735 ns.

**Cache and branch profile** (cachegrind, 1M messages, timed loop only):

| Design point | instructions | D refs | D1 misses | LL misses | branch mispredicts |
|---|---:|---:|---:|---:|---:|
| `ref` | 440.0 M | 217.9 M | 9.19 M | 0.93 M | 7.16 M |
| `aos-scatter` | 176.8 M | 78.0 M | 4.23 M | 1.65 M | 1.18 M |
| **`aos`** | **161.0 M** | **79.7 M** | **3.99 M** | **1.48 M** | **1.19 M** |
| `soa` | 163.9 M | 88.1 M | 8.35 M | 5.22 M | 1.23 M |
| `hybrid` | 159.6 M | 81.6 M | 4.17 M | 1.76 M | 1.24 M |

### 0 → 1: the data structures

The textbook engine pays for its generality everywhere. Every new price level is a red-black tree
node allocation, and every resting order is a `std::list` node plus an `unordered_map` node.
That is 4.9M heap allocations in 5M messages. Finding the next price walks tree pointers.

Design point 1 replaces all of it:

- a **dense price ladder** (level = array index; 16-byte levels, four per line)
- a **two-level bitmap** to find the next best price in O(1) word operations
- a **slab pool** with an embedded LIFO free list
- an **open-addressing id index** with backward-shift deletion

Branch-free crossing checks replace emptiness tests: the empty-book sentinels are -1 and
num_levels.

The result is **−60% instructions, −54% D1 misses, −84% branch mispredicts, zero allocations**.
Yet throughput improved only 18%, and median latency got *worse*. The LL misses explain why: they
went **up** 1.8x.

### 1 → 2: the finding that mattered most

**Problem.** The id index is sized for capacity: 2^19 slots. The textbook hash (Murmur3 fmix64)
scatters ids uniformly over it. The ~4K orders live in a book at any moment are therefore spread
across the whole table, and nearly every lookup (dup check, cancel, fill, modify) touches a cold
cache line and usually a cold page. The `std::map` baseline, whose containers grow only to the
live set, had the smaller working set.

**Fix.** A **locality-preserving hash**: keep the low bits of the id, folded with the bits above
the table size. Ids are issued roughly sequentially, so the live window stays compact and
cache-resident whatever the capacity.

**Result.** **2.3x throughput** (11.4 → 26.3 M msg/s) and **3.4x lower median latency**
(177 → 52 ns). Only −6% D1 / −10% LL in cachegrind. The rest of the win is in the L2 and the dTLB,
which cachegrind does not model. That is exactly why the wall-clock measurement matters.

**Confirming it's footprint, not code.** The capacity sweep below shows the same pattern. At a
capacity small enough for the whole table to fit in L2, scatter and locality converge:

| capacity / book | `ref` | `aos-scatter` | `aos` | `soa` | `hybrid` |
|---:|---:|---:|---:|---:|---:|
| 262,144 | 9.97 | 13.79 | **29.63** | 24.89 | 28.43 |
| 16,384 | 9.49 | 38.33 | **43.73** | 35.63 | 41.38 |

(M msg/s best-of-9, 3M messages.)

**Trade-off.** A client who picks ids strided by the table size can build long
probe chains under `LocalityHash`. That is a latency-DoS vector if ids are client-chosen and
untrusted. Real venues assign order ids themselves, which removes it. Otherwise use `ScatterHash`
(a template argument) or salt the fold. `order_index_locality_survives_strided_keys` tests that the
worst case stays correct.

**Follow-on: 8-byte tagged index slots.** A line-level cachegrind profile (`-O3 -g`, `cg_annotate`)
attributed about **40% of all D1 misses and 40% of LL misses to the id index**. Index slots were
then cut from 16 bytes `{u64 key, u32 slot}` to 8 bytes `{u32 fingerprint, u32 slot}`, with
matches verified against the order record, which every operation touches anyway. Measured on
`aos`: D1 misses 4.16M → 3.99M (−4%), LL misses 1.67M → 1.48M (−11%).

### 2 → 3 / 4: the structure-of-arrays hypothesis, refuted

The premise was that a structure-of-arrays order store is more cache-friendly. **For this workload
it is not.**

**Pure SoA doubles D1 misses (2.1x) and multiplies LL misses by 3.5x.** Matching, cancel and modify each access
*one order* and touch most of its fields: id, qty, next, prev, level, side, owner. In pure SoA
those fields live in nine different arrays, so one order costs up to nine cache lines instead of
one. SoA wins when a loop streams one field across many elements. An order book's access pattern
is the opposite: random access to whole records.

**The hot/cold split was measured twice:**

| Variant | D1 misses vs AoS | LL misses vs AoS | Verdict |
|---|---:|---:|---|
| 16 B `{id, qty, next}` "matching" record + 16 B `{prev, level, owner, side}` "link" record + audit | +20% | +59% | rejected: cancel/modify/insert need both halves |
| 32 B record of every engine-touched field + 16 B cold audit `{ts, orig_qty}` (shipped as `hybrid`) | +5% | +19% | read misses −7%, but the separate audit writes cost more than that saves |

**Wall-clock doesn't separate the three layouts.** Best-of-15 spans 24.1–26.3 M msg/s and the
medians invert the order. The extra D1 misses in SoA mostly hit L2, which is cheap. `aos` has the
fewest simulated misses and the best best-of-N, so it is the default. The other two stay in the
suite so nobody has to take the folklore on faith.

## Replay determinism

`exsim_gen` writes a 10M-message binary capture (400 MB). `exsim_replay` pushes it through each
implementation:

| book | events | trades | digest | replay time |
|---|---:|---:|---|---:|
| ref | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 1.42 s |
| aos | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 0.89 s (11.3 M msg/s) |
| soa | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 1.03 s |
| hybrid | 13,134,237 | 2,936,622 | `73aad034f17dc493` | 1.18 s |

The threaded pipeline (below) prints the same digest. Threading, decoding and memory layout
never change results.

## SPSC ring

Two pinned threads moving the 56-byte `Command`: **42.4 M items/s** (checksum-verified).

## Pipeline: feed → ring → engine → ring → market data

Three pinned threads. The engine thread also computes top-of-book after every message, publishes
BBO changes and trades (4.8M market-data messages per 10M inputs), and stamps a latency sample per
message.

**Saturated** (feed unthrottled): **4.71 / 4.98 / 5.08 M msg/s** over three runs. The engine
thread is the bottleneck: the ingress ring is full throughout, and the market-data ring never
backs up. It runs below single-threaded replay because every command now arrives on a cache line
freshly written by another core. A consumer-side prefetch of already-published ring slots
(`SpscQueue::prefetch`) recovered **+11–15%**, measured as an interleaved A/B (mean 4.09 → 4.72 M
msg/s).

**Paced** (ingress stamps are *scheduled* send times, which corrects for coordinated omission):

| offered load | ingress → match p50 | p90 | p99 | ingress → md consumed p50 |
|---:|---:|---:|---:|---:|
| 0.5 M msg/s | 220 ns | 8.8 µs | 1.08 ms | 453 ns |
| 1.0 M msg/s | 208 ns | 15.3 µs | 1.05 ms | 429 ns |
| 2.0 M msg/s | 214 ns | 65.9 µs | 3.62 ms | 429 ns |

The p50 is the true cost: decode, cross-core hop, matching and bookkeeping in ~210 ns, and ~430 ns
through a second hop to the market-data consumer. The p90 and above track the jitter probe. With
the engine idle more than 90% of the time at 0.5M msg/s, a millisecond p99 can only come from the
core being taken away. On an isolated core (`isolcpus`, `nohz_full`, IRQ affinity) the tail should
collapse toward the engine's own p99.9 (~2.3 µs). That is untested here, and stated as a
hypothesis.

## Reproducing

```bash
scripts/bench.sh                 # build, design points, SPSC, replay determinism, pipeline
scripts/cache_profile.sh         # perf counters if available, else cachegrind
./build/release/exsim_pipeline --jitter 10    # how noisy is this machine?
./build/release/exsim_bench --max-orders 16384 --impl aos-scatter,aos   # the footprint effect
```
