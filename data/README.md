# Market data

`sample_btcusdt_30s.exmd` is 30 seconds of real Binance BTCUSDT level-2 data (301 depth diffs, 879 trades and 10
REST snapshots), small enough to commit (0.5 MB). It lets CI and any reader run the real-data validation without
downloading anything:

```bash
build/release/exsim_mdreplay --in data/sample_btcusdt_30s.exmd
```

The longer sessions behind the market-making study (30 min of BTCUSDT, 10+ min of ETHUSDT) are tens of MB and are not
committed. Re-capture your own with the scripts below (Binance's public market-data endpoints, no API key). The results in
`results/` were produced from those captures, and the grid is deterministic given a capture.

## Capturing and converting

```bash
pip install websockets
python scripts/capture_binance.py --symbol BTCUSDT --minutes 30 --out data/btcusdt.jsonl   # JSONL, one record per line
python scripts/mdconv.py data/btcusdt.jsonl data/btcusdt.exmd                              # exact integer ticks / lots
```

## The `.exmd` format

All decimal scaling happens once, in `mdconv.py`, with exact decimal arithmetic (it refuses values that are not
exactly representable). Prices are integer ticks (1e-2 for BTCUSDT) and quantities integer lots (1e-5), so nothing
downstream touches floating point.

```
header  "EXMD" | u32 version=1 | u32 price_decimals | u32 qty_decimals
record  u32 payload_len | u8 kind | u8 pad[3] | u64 rx_ns | body        (little endian)
  kind 1 diff      u64 first_id, u64 last_id, u32 n_bids, u32 n_asks, then {i64 px, u64 qty} x (n_bids + n_asks)   qty 0 = delete
  kind 2 trade     i64 px, u64 qty, u8 buyer_is_maker
  kind 3 snapshot  u64 lastUpdateId, u32 n_bids, u32 n_asks, then {i64 px, u64 qty} x (n_bids + n_asks)
```

`rx_ns` is the **local receive time**, and it is the only clock used for everything. Mixing the exchange's trade
timestamps with local book-update timestamps skews every fill-relative measurement (the two clocks differ by seconds),
so the capture never uses exchange timestamps.

Diffs and trades arrive on the same websocket connection, so `rx_ns` orders them correctly relative to each other.
Snapshots are fetched over REST every 2 s **after** the websocket is open (Binance's documented procedure), and their
`lastUpdateId` lines up exactly with a diff boundary in every case observed, which is what makes exact validation possible.
