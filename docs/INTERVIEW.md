# Questions this project should be able to answer

Each answer points at the code, test or measurement that backs it. If you cannot answer one of these from memory,
read the linked file before presenting the project. The answers include what is *not* established.

## The engine

**Walk me through a market buy arriving.**
`OrderBook::add` validates (id, qty, price band, duplicate), emits `Accepted`, then `sweep` runs: while the order has
quantity and `best_ask_ <= limit` (for a market order, `limit` is the top of the band), take the head of the best level's
FIFO, emit a `Trade` at the maker's price, decrement or release the maker, and when a level empties clear its bit and find
the next best with `PriceBitmap::find_next`. A market order's unfilled remainder is cancelled (`IocExpired`), never rested.
See `include/exsim/order_book.hpp`, `sweep`.

**Why is cancel O(1)?**
Cancel finds the slot through the id index (one probe in the common case), then unlinks it from an intrusive doubly linked
FIFO: two pointer writes. If the level empties, one bit clears. Nothing is searched or shifted.

**Why a bitmap for the next best price?**
After the best level empties, the next non-empty level can be arbitrarily far away (the BTC book is sparse: gaps of hundreds
of ticks). A two-level bitmap finds it with at most two word scans plus a scan of a 16-word summary, independent of the gap.
`bitmap_matches_std_set` checks it against `std::set` over 50,000 random operations per size.

**Your first optimized version was slower than `std::map`. Why?**
It had 60% fewer instructions and 84% fewer branch mispredictions, so the problem was memory. The id index was sized for
capacity (8 MiB) but held ~4,000 live orders, and a well-mixed hash scattered them across all of it, so nearly every lookup
touched a cold cache line and page. A hash that keeps live sequential ids near each other gave 2.3x. See DESIGN.md section 4.

**That hash sounds attackable.**
It is. Keys crafted against the `k ^ (k >> bits)` fold all land in one home slot and lookups go from ~1.5-3 ns to ~770-950 ns (300-500x, two campaigns)
(`exsim_bench --adversarial`; the test `order_index_locality_hash_stays_correct_under_a_worst_case_collision_attack` shows
correctness survives, only speed suffers). It is safe when the exchange assigns order ids. `fmix64` is not a fix either: it is
invertible, so an informed attacker can still craft collisions. A gateway taking client ids needs a keyed hash. The current
gateway forwards client ids, which is a documented limit.

**Why did structure-of-arrays lose?**
Every operation touches most fields of *one* order, so SoA turns one cache line into up to nine. Cachegrind: 2.1x the L1
misses and 3.5x the last-level misses of the plain record layout. SoA wins when a loop streams one field across many
elements; an order book is random access to whole records.

**Which layout is fastest?**
Not established by wall-clock: `aos`, `soa` and `hybrid` have overlapping bootstrap intervals (about 2.6x, 2.5x, 2.8x over the
baseline). `aos` is the default because it has the fewest simulated misses, which is deterministic; that is a weaker claim than
"fastest", and the docs say so.

## Correctness

**How do you know the engine is right?**
Four independent lines of evidence:
1. Differential testing: identical random streams to a naive `std::map` book and to every optimized variant; events must be
   byte-identical after every command, and full book state is compared every 50 commands (1.7M events, 120 seeds, all three
   self-trade policies).
2. Mutation testing: I planted four bugs by hand; each was caught (one by an invariant audit, one by a snapshot divergence,
   one by an event divergence, one by unit tests).
3. Cross-check against Liquibook, an independent engine: 933,619 trades and 96,761,274 lots, identical, on 3M commands.
4. Real data: the reconstructed Binance book equals the exchange's own snapshots (573/573, 458,400 levels), and fault
   injection proves the check can fail.

**What are the tests *not* covering?**
The reference book and the fast books share my understanding of the rules. Agreement with Liquibook covers Day/IOC limit
orders and cancels only (it lacks post-only, FOK, modify and STP), so those features are covered by the reference book and
by hand-written scenarios, not by a third-party oracle.

## Concurrency and durability

**What does the SPSC ring guarantee and why is acquire/release enough?**
One producer, one consumer. The producer's release-store of `tail_` publishes the slot write; the consumer's acquire-load
of `tail_` sees it. The consumer's release-store of `head_` frees the slot for reuse; the producer's acquire-load sees that.
No total order across threads is needed, so no `seq_cst`. TSan over the full three-thread pipeline reports nothing, and a
5M-item ordered stress test passes.

**Why 128 bytes between head and tail, not 64?**
Intel's L2 spatial prefetcher fetches adjacent 64-byte lines in pairs, so data written by two threads within 128 bytes can
still ping-pong.

**What does your journal guarantee after a crash?**
Write-ahead plus flush-before-ack: any command a client saw acknowledged is in the journal. Torn tails are detected and
discarded; corrupt records stop the read and distrust everything after. `--sync os` survives a process crash, not power
loss; `batch`/`every` add `fsync`. I tested a real `kill -9` under load and compared the recovered digest with an independent
offline replay. I did **not** test power loss or disk failure.

## The benchmark environment

**These numbers are from a laptop under WSL2. Why trust them?**
Trust ratios within a run, not absolutes. A jitter probe shows the hypervisor taking ~6-12% of a pinned core, up to 10 ms at a
time, so wall-clock throughput drifts up to 2x between runs and tail latency above ~p99 reflects the hypervisor. Mitigations:
interleaved repetitions, best-of-N *and* median with bootstrap confidence intervals, a deterministic cache simulator for layout
questions. What is missing: bare-metal runs with isolated cores and hardware counters.

**Is 17x over Liquibook a fair claim?**
It is a real measurement on identical input with identical results, but not a like-for-like feature comparison. Liquibook
supports stop orders, depth tracking and per-operation callbacks, and stores levels in a `std::multimap`. Read it as "a
conventional design costs this much on this workload", not "Liquibook is slow".

## The market-making study

**Why does every strategy lose money?**
On these captures, at zero fees, with a queue-aware fill model and 10 ms latency, passive quotes are filled disproportionately
when the price is about to move against them (on BTC the mid moves $4.7 to $10 against a fresh fill within 1 s, against an unconditional 1 s standard deviation of $3.49 and mean absolute move of $1.12). Spread capture
is cents; inventory loss is dollars. Fees make it worse: at 10 bps every strategy loses $55 to $420 in half an hour.

**Does Avellaneda-Stoikov beat a fixed spread?**
Against an equal-width fixed spread, only at high risk aversion, and only by trading much less (0.58 BTC vs 1.99): it loses
less, it does not earn more. At lower risk aversion the paired 95% interval includes zero. Interpret it as inventory control,
not edge. See RESEARCH.md for the intervals and for what one 30-minute trending session cannot tell you.

**What would change your conclusion?**
A different regime (this BTC session trended up), maker rebates (I test -0.5 bps but a real rebate tier depends on volume),
a lower-latency queue position than my model assumes, or evidence that my cancellation proration (a neutral assumption; a
canceller's queue position is unobservable in L2 data) is systematically biased.

**How does the fill model work, and where is it weakest?**
Virtual quotes join the back of the displayed queue. Trades at the price consume the queue ahead first; trades through the
price fill in full; unexplained level shrinkage is cancellation, prorated by queue share. Weakest points: no own market
impact, no hidden/iceberg orders, book data at 100 ms granularity (so "mid at fill" can be up to 100 ms stale), and the
proration assumption. The optimistic-fill comparison quantifies the model's importance: for touch quoting a naive backtest
counts ~7x the fills.

## Process

**What bug taught you the most?**
The failing real-data validation. It looked like a mirror bug (147 of 573 snapshots failed). The actual cause was a property of
diff-based reconstruction: unchanged levels deeper than the seed snapshot are invisible. Fixing it meant defining exactly
what the mirror can vouch for, and reseeding when the market outgrows that region. The lesson was to distrust a failing check
long enough to find out *why*, not to loosen it.
