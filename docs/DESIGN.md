# Design notes

Why the system is built the way it is, what was tried, and what the evidence says. Numbers reference
[BENCHMARKS.md](../BENCHMARKS.md) and [RESEARCH.md](RESEARCH.md).

## 1. Scope

**Goals.** A correct price-time priority matching engine whose hot path never allocates; a deterministic,
replayable event stream; an order-entry gateway that cannot lose an acknowledged command; validation against
real exchange data; a market-making study on top of it that reports what does *not* work as plainly as what does.

**Non-goals.** Multi-venue routing, auctions, derivatives, FIX, cross-machine replication. Some are listed under
[Limits](#9-limits-and-next-steps).

## 2. Shape of the system

```
                       +--------------------- deterministic core ----------------------+
 client --TCP--> gateway --> sequencer --> journal (write-ahead) --> risk gate --> engine --> events
  (binary)      decode      seq, owner, ts    CRC32C, append-only     limits        per-symbol   |
                                                                                    books        v
                                                                            exec reports / market data
```

Everything inside the box is a **pure function of the ordered command stream**. That single property gives, for free:

- *Crash recovery.* Replay the journal into a fresh engine and you have the pre-crash state, exactly.
- *Verification.* Two implementations fed the same stream must emit identical events (differential testing).
- *Reproducibility.* A bug report is a journal file. A benchmark is a capture. A study is a capture.

The rules that protect that property: no wall-clock reads inside the core (the gateway stamps `ts` once, and the
journal records it); no unordered-container iteration that affects output; no floating point in matching.

## 3. The matching engine

| Concern | Choice | Alternative | Evidence |
|---|---|---|---|
| Price levels | dense array indexed by `price - min_price`; 16 B levels | `std::map` of levels | A level lookup is one address computation. Tree walks pointer-chase and allocate. |
| Next best price | two-level bitmap (`countr_zero` / `countl_zero`) | scan for non-empty | Constant-time after the best level empties; property-tested against `std::set`. |
| Empty-side test | sentinels `-1` and `num_levels`; "does it cross?" is one signed compare | `optional`/emptiness branch | Removes a branch from every match iteration. |
| FIFO per level | intrusive doubly linked list of 32-bit slot indices | `std::deque`, `std::list` | Cancel is O(1) unlink. The head's `prev` is never read, so popping the head writes one field. |
| Order storage | slab with an embedded LIFO free list, prefaulted, 2 MiB-aligned (THP) | per-order `new` | Zero allocations in the hot path (a test counts them). LIFO reuses the cache-warm slot. |
| Id lookup | open addressing, linear probing, backward-shift deletion, 8 B `{fingerprint, slot}` | `unordered_map`, tombstones | See section 4: this is where the largest single speedup came from. |
| Event delivery | `Sink` template parameter | virtual `IEventHandler` | No indirect call per event; an ignored event costs nothing. |

### Semantics worth being precise about

- **Trades execute at the resting order's price**, in price then arrival order.
- **Modify.** Shrinking size at the same price keeps queue priority (it is an in-place amend). A reprice or size
  increase is cancel/replace: it goes to the back of the queue and may trade immediately. The UI lets you see both.
- **FOK** is all-or-nothing and is decided *before* any fill, using level aggregates (or a walk of the queue when
  self-trade prevention is on, because your own resting orders then provide no liquidity).
- **Self-trade prevention** has two modes (`CancelResting`, `CancelIncoming`); the differential test runs all three
  policies including off.
- **Capacity is a hard limit.** When the order store is full the remainder is cancelled with `BookFull`. The engine
  never grows, never allocates, and never throws on the hot path.

## 4. What the measurements changed

The first optimized engine was **slower** than the `std::map` baseline (0.74 to 0.85x). Instruction count and branch
mispredictions had dropped by 60% and 84%, so the cause had to be memory.

Line-level cache profiling (cachegrind with debug info) attributed about 40% of all misses to the id index. It was sized
for *capacity* (2^19 slots, 8 MiB) but held only ~4,000 live orders, and a textbook scattering hash spread those orders
across the whole table: nearly every lookup touched a cold line and a cold page.

Two fixes, in order of impact:

1. **Locality-preserving hash** (`k ^ (k >> bits)`): ids are issued roughly sequentially, so live orders occupy a
   compact window of the table whatever its capacity. 2.3x throughput. The trade-off is real and measured: keys crafted
   against the fold degrade lookups 300-500x (767-953 ns vs 1.5-3 ns), so this hash is only safe where the *exchange* assigns
   order ids. A gateway accepting arbitrary client ids should use a keyed hash. (Plain `fmix64` is not safe against an
   informed attacker either: it is invertible.)
2. **8-byte tagged slots**: `{u32 fingerprint, u32 slot}` instead of `{u64 key, u32 slot}`, verified against the
   order record that every operation touches anyway. Halves the index's cache footprint: -11% last-level misses.

**Refuted: structure-of-arrays.** Pure SoA *doubled* L1 misses and multiplied last-level misses by 3.5. Matching, cancel
and modify each touch most fields of one order, so SoA turns one cache line into up to nine. Two hot/cold splits were
also measured and rejected (+59% and +19% LL misses vs plain records). Wall-clock cannot separate the three layouts
(their bootstrap intervals overlap), so the default (`AosBook`) rests on the deterministic cache simulation, and the
other two stay in the suite.

Caveat that applies to all of it: cache numbers come from a simulator (no L2, TLB or prefetcher), and the machine is a
WSL2 laptop where the hypervisor steals ~6-12% of a pinned core. Ratios within a run are solid; absolute numbers move.

## 5. Concurrency

Three pinned threads joined by SPSC rings: feed (decode, stamp) -> engine -> market-data publisher.

- **Ring.** Power-of-two capacity with free-running 64-bit indices (full/empty is subtraction; no wasted slot);
  acquire/release only, no seq_cst, no CAS; each side caches the other's index so steady state touches no shared
  line; producer and consumer lines are 128 B apart because the L2 adjacent-line prefetcher pulls lines in pairs;
  consumer-side prefetch of already-published slots gave +11-15% pipeline throughput (interleaved A/B).
- **Correctness of the ring** is checked by an in-order two-thread stress test (5M items) and by ThreadSanitizer over
  the full pipeline (0 reports).
- **Latency is measured against scheduled send times** (open loop), so a slow consumer shows up as latency instead of
  silently throttling the producer. Cross-core timestamp deltas can be negative under virtualization; they are clamped
  and counted, not wrapped.

## 6. Durability and recovery

- **Write-ahead.** The gateway appends to the journal *before* the engine sees a command, and flushes it before any
  response for that batch leaves the process. A client therefore never holds an acknowledgement for something a crash
  can erase.
- **Format.** `len | crc32c | ts | owner | payload`. The wire format omits the ingress timestamp and (for cancels and
  modifies) the owner, but risk decisions depend on both, so the record stores them and the CRC covers them.
- **Failure handling.** A short final record (crash mid-write) is a *torn tail*: discarded, and recovery truncates the
  file so appends resume cleanly. A record that fails its CRC is *corruption*: reading stops there and everything after
  it is distrusted, even if it happens to parse.
- **What is tested:** a real `kill -9` of the server under load, restart with `--recover`, then (1) no acknowledged
  command is missing, (2) the recovered digest equals an independent offline replay, (3) a deliberately truncated
  tail is detected. `scripts/e2e_gateway.sh` runs all of it.
- **Sync policy is explicit:** `os` survives a process crash; `batch`/`every` add `fsync` for power loss. The default
  is the fast one, and the docs say so.

## 7. Reconstructing a real exchange book

Exchanges publish level totals; the engine matches orders. The mirror represents each price level as one synthetic order
whose quantity is the level total (ids derived from price and side, so no lookup table).

Two subtleties, both found by testing rather than by reasoning first:

1. **Removals before additions.** A 100 ms diff can move the whole book. Applying additions first makes the batch
   transiently cross and the engine generates phantom trades (109 on the 30-second sample alone when the order is
   reversed, plus 2 failed snapshots).
   Removals and reductions are applied across both sides first; phantom trades stay at 0.
2. **Completeness horizon.** A diff stream only reports levels that *change*. A level deeper than the seed snapshot that
   never changes is invisible forever, so the mirror is provably complete only between the touch and the snapshot's
   deepest level. The first validation run failed 147 of 573 snapshots for exactly this reason, when the market
   rallied ~$170 and the compared window slid past the seed's horizon. The fix: compare only inside the provable
   region, and reseed from an exactly-aligned snapshot once half the seeded depth is consumed. Result: 573/573 exact.

Validation compares the reconstructed book with the exchange's own snapshot at the exact sequence point
(`last_update_id`), level for level. It is fault-injection tested: dropping 6 of 648,000 level updates fails 8 snapshots.

## 8. Pre-trade risk

Per-order size and notional caps, a price collar around the last trade, a per-owner generic-cell-rate limiter, symbol
and global halts. Cancels are never blocked (a participant must always be able to reduce risk). Rate limiting uses the
journaled `ts`, so a replay makes every accept/reject decision identically (tested on 30,000 commands).

## 9. Limits and next steps

- **No bare-metal numbers.** WSL2 hides the PMU and the hypervisor injects jitter. Next: `isolcpus`/`nohz_full`,
  `perf stat` hardware counters, and the same grid on real silicon.
- **One matching thread per shard.** Symbols are independent, so sharding is straightforward, but no router exists.
- **No replication.** The journal is the input to a hot standby; the standby is not built.
- **Client-chosen ids** must not reach the locality hash (see section 4). The gateway currently forwards them.
- **The market-making study is one hour of data across two assets.** Intervals are wide; see RESEARCH.md for what is
  and is not established.
