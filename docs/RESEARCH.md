# Passive market making on real order-book data

**Question.** Can a passive quoting strategy make money on liquid crypto spot books once fills are modeled honestly (queue
position, latency) and fees are included, and does the Avellaneda-Stoikov (A-S) model beat simpler quoting?

**Short answer.** No, not on these data. Every strategy loses about 1.3 to 2 basis points of the notional it trades, because
the fills it gets are disproportionately the ones where the price is about to move against it. A-S is an inventory-control
tool: it loses less by trading less, and its edge over an equal-width fixed spread is statistically indistinguishable from
zero in all but one cell. A naive "touch means filled" backtest overstates fills by up to 7x. Fees at retail levels swamp
everything.

That is a result about ~1 hour of data on two assets, not a law. The [limits](#what-this-does-not-show) section is part of
the result.

## Data

Live Binance spot data over public endpoints, recorded by `scripts/capture_binance.py` against **one local clock**: 100 ms
depth diffs, every trade, and a REST snapshot every 2 s (websocket opened first, per Binance's book-management procedure).

| Session | Asset | Length | Diffs | Trades | Snapshots checked | Book validation |
|---|---|---:|---:|---:|---:|---|
| `btcusdt_a` | BTCUSDT | 30 min | 17,997 | 57,885 | 573 | 573/573 exact, 458,400 levels |
| `ethusdt_b` | ETHUSDT | 10 min | 6,028 | 12,655 | 168 | 168/168 exact, 134,400 levels |
| `ethusdt_c` | ETHUSDT | 25 min | 14,951 | 55,055 | 418 | 418/418 exact, 334,400 levels |

The engine's reconstructed book equals the exchange's own snapshot, level for level (price and quantity), at the exact
sequence point (`lastUpdateId`), with 0 phantom trades and 0 sequence gaps, in every session. The check is shown to be able
to fail: dropping 1 in 100,000 level updates (6 of 648,000) fails 8 BTC snapshots; 1 in 2,000 fails 441 of 573. Details and
the two ways it initially failed are in [DESIGN.md section 7](DESIGN.md#7-reconstructing-a-real-exchange-book).

## Method

**Book.** The exchange book is replayed through the matching engine (one synthetic order per price level). Quotes are
*virtual*: they are never inserted, because level quantities are absolute totals and inserting our size would double-count it.

**Fill model** (`include/exsim/mm.hpp`, unit-tested on hand-computed cases):
- On going live a quote joins the **back** of the displayed queue: `ahead = level quantity`.
- A trade at our price consumes `ahead` first; only the excess fills us. A trade *through* our price fills us in full.
- Level shrinkage that trades do not explain is cancellation. A canceller's queue position is unobservable in L2 data, so it
  is prorated by queue share: `ahead -= cancelled * ahead / (level - traded)`. Level growth joins behind us.
- If the market moves to or through our price we are filled in full.
- `--fill optimistic` ignores the queue (any trade at or through the price fills us), to measure how much that overstates.

**Latency** is decision-to-market, both for placement and for cancels, with amend-in-flight (a re-quote before the previous
one is live keeps the original effective time). **Fees** are basis points of notional per fill; negative is a rebate.
**Re-quote hysteresis** (2 ticks) stops strategies from forfeiting queue position on 1-tick wobbles.

**Strategies.**
- *Avellaneda-Stoikov*: reservation `r = mid - q*gamma*sigma^2*tau`, half spread `0.5*gamma*sigma^2*tau + ln(1 + gamma/k)/gamma`,
  `sigma^2` an EWMA variance rate (10 s half-life), `tau` a fixed 30 s horizon (a constant, the standard adaptation for a
  continuously running desk), inventory in quote-size units, capped at 10 units.
- *Fixed spread* at `h` ticks each side, same size and cap. One is set to `1/k` ticks, **as wide as A-S's own quotes**: the
  fair control, since otherwise A-S's width, not its inventory skew, would drive any difference.
- *Join touch*: quote the best bid and ask.

**Calibrating k.** A-S assumes the rate a quote at distance `d` from the mid is hit is `A*exp(-k*d)`. A quote at distance `d`
is reached by every trade that hits a resting order at depth >= `d`, so `lambda(d)` is the empirical survival rate of trade
depth. `scripts/calibrate_k.py` fits it from the same capture:

| Session | k (per tick) | 1/k | exponential R^2 | median / p90 trade depth (ticks) |
|---|---:|---:|---:|---|
| BTCUSDT | 0.00243 | 411 | 0.98 | 0.5 / 526 |
| ETHUSDT (10 min) | 0.0584 | 17 | 0.996 | 1.5 / 25.5 |
| ETHUSDT (25 min) | 0.0289 | 35 | 0.95 | 4.5 / 39.5 |

k differs 12- to 24-fold between BTC and ETH (ETH's fixed $0.01 tick is ~30x coarser relative to price), and **by 2x between two
sessions of the same asset**, so a calibrated k is a snapshot of a regime, not a constant. The BTC fit is good in the tail but
the measured intensity is flat out to ~50 ticks (`docs/img/k_calibration_btcusdt_a.png`): A-S's exponential form is a local
approximation, and trade depth is clustered (one sweep produces many fills), which a Poisson model ignores.

**Grid, fixed before looking at results.** Risk aversion is swept in the dimensionless ratio `gamma/k` in
{4e-4, 4e-3, 4e-2, 4e-1} so different tick sizes get the same principled grid; latency in {0, 10, 50, 200} ms; fees in
{+10, +1, 0, -0.5} bps; fill model in {queue, optimistic}. All 72 runs per session are in `results/*/grid.csv`.

**Statistics.** PnL increments are strongly autocorrelated, so a naive standard error would be far too small. Intervals are
**moving-block bootstrap** on 30-second increments (4,000 resamples); for a strategy difference the blocks are **paired**
(same time windows for both). The accounting is audited by `scripts/verify_accounting.py`, which recomputes every run's PnL,
inventory and spread capture from the raw fill logs with no shared code; all runs reconcile to within 0.02 USDT (0.0001 on BTC).

## Results

Main comparison: 10 ms latency, no fees, queue-aware fills, 95% block-bootstrap interval on total PnL (USDT).

### BTCUSDT, 30 minutes (quote size 0.01 BTC)

| Strategy | Fills | Volume (BTC) | Spread capture | Inventory PnL | **Total** | 95% CI | bps of notional | Mean \|inv\| (BTC) |
|---|---:|---:|---:|---:|---:|---|---:|---:|
| touch | 1,246 | 4.39 | -0.29 | -51.25 | **-51.54** | [-76.3, -27.7] | -1.40 | 0.074 |
| fixed h=2 | 512 | 2.97 | -0.32 | -44.19 | **-44.51** | [-71.8, -18.8] | -1.78 | 0.067 |
| fixed h=10 | 297 | 2.91 | -0.09 | -45.00 | **-45.08** | [-72.0, -19.2] | -1.84 | 0.067 |
| fixed h=100 | 297 | 2.76 | 2.37 | -39.46 | **-37.09** | [-62.2, -14.2] | -1.60 | 0.065 |
| fixed h=412 (= A-S width) | 201 | 1.99 | 7.45 | -34.17 | **-26.72** | [-46.1, -9.1] | -1.60 | 0.055 |
| A-S gamma/k=4e-4 | 217 | 2.01 | 7.46 | -32.09 | **-24.63** | [-44.4, -6.5] | -1.46 | 0.050 |
| A-S gamma/k=4e-3 | 231 | 2.02 | 7.25 | -32.47 | **-25.22** | [-44.4, -10.2] | -1.48 | 0.033 |
| A-S gamma/k=4e-2 | 164 | 1.31 | 4.26 | -22.45 | **-18.19** | [-27.1, -11.2] | -1.65 | 0.012 |
| A-S gamma/k=4e-1 | 66 | 0.58 | 1.96 | -8.92 | **-6.96** | [-9.9, -4.4] | -1.44 | 0.005 |

Paired difference, A-S minus the equal-width fixed spread (fixed h=412), total PnL over the session:

| A-S gamma/k | difference (USDT) | 95% paired CI |
|---:|---:|---|
| 4e-4 | +2.1 | [-4.5, +7.8] |
| 4e-3 | +1.5 | [-12.0, +13.6] |
| 4e-2 | +8.5 | [-6.4, +23.5] |
| 4e-1 | **+19.8** | **[+2.5, +38.2]** |

### ETHUSDT, 25 minutes (quote size 0.01 ETH, about $27)

| Strategy | Fills | Spread capture | Inventory PnL | **Total** | 95% CI | bps of notional |
|---|---:|---:|---:|---:|---|---:|
| touch | 1,438 | 0.03 | -3.04 | **-3.01** | [-5.0, -1.4] | -1.31 |
| fixed h=2 | 1,439 | 0.10 | -3.64 | **-3.53** | [-5.8, -1.7] | -2.09 |
| fixed h=10 | 914 | 0.41 | -2.89 | **-2.48** | [-4.6, -0.7] | -1.97 |
| fixed h=35 (= A-S width) | 192 | 0.38 | -2.14 | **-1.75** | [-3.9, +0.1] | -5.19 |
| fixed h=100 | 11 | 0.10 | -0.33 | **-0.22** | [-1.0, +0.5] | -7.49 |
| A-S gamma/k=4e-4 | 227 | 0.40 | -2.10 | **-1.70** | [-3.5, -0.2] | -4.60 |
| A-S gamma/k=4e-3 | 221 | 0.41 | -1.33 | **-0.92** | [-1.9, -0.2] | -2.42 |
| A-S gamma/k=4e-2 | 95 | 0.21 | -0.48 | **-0.27** | [-0.5, -0.1] | -1.34 |
| A-S gamma/k=4e-1 | 27 | 0.06 | -0.14 | **-0.09** | [-0.16, -0.03] | -1.49 |

Every paired A-S-vs-fixed(35) interval includes zero (best case +1.7, CI [-0.2, +3.8]). The 10-minute ETH session shows the same
pattern (`results/ethusdt_b/`).

## Findings

1. **Passive quoting loses, and the loss is adverse selection, not bad execution.** Spread capture is positive but tiny
   (cents to a few dollars); inventory PnL is negative in every run. One second after a BTC fill the mid has moved against the
   position by $4.7 to $10. Unconditionally, BTC's 1-second mid change in this session has a standard deviation of $3.49 and a
   mean absolute value of only $1.12, so a fresh fill is followed by a move 4 to 9 times the typical one, in the wrong
   direction. Thirty seconds after a fill the mid is $9 to $14 against the position (unconditional 30 s standard deviation:
   $25). Fills are not a random sample of time: they are the moments someone aggressive chose to trade.

2. **The loss per dollar traded is nearly design-independent.** On BTC every strategy loses 1.4 to 1.8 bps of notional; on
   ETH the near-touch strategies lose 1.3 to 2.1 bps. Deep quotes on ETH show larger bps (4 to 7) but on 11 to 200 fills, so they
   are noisy. It follows that only a **maker rebate above roughly 1.5 bps** or a genuine information edge could make this
   profitable; the tested -0.5 bps rebate still loses on every strategy and asset.

3. **Fees dominate.** At 10 bps of notional (Binance's default retail maker rate) BTC losses become $55 (A-S at high risk
   aversion) to $421 (touch quoting) in 30 minutes. At 1 bps they raise the zero-fee loss by roughly 60 to 70%.

4. **A-S wins by trading less, not by earning more.** Its inventory is far smaller (mean 0.005 to 0.05 BTC vs 0.055 to 0.074)
   and its total loss is smaller at high risk aversion, but at gamma/k = 4e-1 it also trades 0.58 BTC against 1.99. On BTC that
   cell is the only significant improvement over an equal-width fixed spread (+19.8, CI [+2.5, +38.2]). With 4 gamma values
   and 2 assets, some cell being nominally significant is not surprising, and it is not replicated on ETH (CI includes 0).
   Read it as risk reduction.

5. **A naive fill model overstates activity, and sometimes results.** For touch quoting, "any trade at the price fills me"
   yields 8,693 BTC fills against the queue-aware 1,246 (7.0x) and a PnL of -41.4 against -51.5. On ETH: 5,281 vs 1,438 fills
   (3.7x). For fixed quotes 10 or more ticks back the two models agree (the queue ahead is short or empty). For actively
   re-quoting A-S at high risk aversion the optimistic model still gives 53% more fills (251 vs 164 on BTC), because the
   queue-aware model charges for the queue position lost at every re-quote. The model matters most where retail backtests
   tend to quote: at the touch.

6. **Latency barely matters here, which is itself informative.** Total PnL changes by a few dollars from 0 to 200 ms decision
   latency (touch: -51.3 at 0 ms, -51.5 at 10 ms, -51.7 at 50 ms, -44.3 at 200 ms; the 200 ms improvement is fewer fills, not
   better ones). The strategies react to 100 ms book updates, so extra delay mostly stales quotes that were adversely selected
   anyway. A strategy that predicted short-horizon flow would be the case where latency matters, and this study has none.

7. **A calibrated parameter is a regime, not a constant.** k moved 25x between assets and 2x between two ETH sessions.

## What this does not show

- **One hour of data, two assets.** The BTC session trended up. A different regime (range-bound, or a volatility spike) could
  change the sign of inventory PnL. Intervals are wide; three of nine BTC strategies have a CI that comes near zero.
- **Model risk in the fill model.** The cancellation proration is a neutral assumption. No hidden or iceberg orders, no own
  market impact (our fills would have moved the book that produced them), book data is 100 ms granular (so "mid at fill" can be
  stale by up to 100 ms), and queue position is estimated, not observed. The optimistic-vs-queue gap brackets the effect
  but does not bound it.
- **Grid, not optimization.** gamma/k was swept and every cell is reported; nothing was tuned on the data. But the strategy
  set is small and there is no signal. This is a study of *quoting policy*, not of alpha.
- **Fees are a constant per fill.** Real tiers depend on volume.
- **Single venue, no hedging.** Inventory is marked to mid and never flattened.

## Reproducing it

```bash
pip install websockets numpy pandas matplotlib
python scripts/capture_binance.py --symbol BTCUSDT --minutes 30 --out data/btcusdt_a.jsonl
python scripts/mdconv.py data/btcusdt_a.jsonl data/btcusdt_a.exmd
build/release/exsim_mdreplay --in data/btcusdt_a.exmd                     # exact validation
build/release/exsim_mmsim --in data/btcusdt_a.exmd --calibrate depth.csv
python scripts/calibrate_k.py depth.csv --plot docs/img/k.png             # -> k
python scripts/run_experiments.py --bin build/release/exsim_mmsim --data data/btcusdt_a.exmd:0.00243 --out results/btcusdt_a
python scripts/verify_accounting.py --results results/btcusdt_a --dataset btcusdt_a
python scripts/analyze_results.py --results results/btcusdt_a --img docs/img --dataset btcusdt_a
```

A fresh capture will not reproduce these numbers exactly (the market is different); the committed `results/` and the
committed 30-second sample let every stage be re-run without network access.
