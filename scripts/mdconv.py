#!/usr/bin/env python3
"""Convert a capture_binance.py JSONL file to the compact binary format read by the C++ tools.

All decimal scaling happens here, once, with exact decimal arithmetic: prices become integer ticks
and quantities integer lots, so nothing downstream touches floating point. The conversion refuses to
proceed if a value is not exactly representable at the chosen scale.

Binary layout (little endian):
  header   char magic[4]="EXMD"  u32 version=1  u32 price_decimals  u32 qty_decimals
  record   u32 payload_len   u8 kind   u8 pad[3]   u64 rx_ns   <kind-specific>
    kind 1 diff : u64 U  u64 u  u32 nb  u32 na   then (nb+na) x { i64 px  u64 qty }   (qty 0 = delete level)
    kind 2 trade: i64 px  u64 qty  u8 buyer_is_maker  u8 pad[7]
    kind 3 snap : u64 lastUpdateId  u32 nb  u32 na   then (nb+na) x { i64 px  u64 qty }

    python mdconv.py data/btcusdt.jsonl data/btcusdt.exmd [--price-dec 2 --qty-dec 5]
"""
import argparse, json, struct, sys
from decimal import Decimal


def scaled(s: str, dec: int) -> int:
    d = Decimal(s).scaleb(dec)
    i = int(d)
    if d != i:
        sys.exit(f"value {s} is not exact at {dec} decimals; choose a finer scale")
    return i


def levels(rows, price_dec, qty_dec):
    return b"".join(struct.pack("<qQ", scaled(p, price_dec), scaled(q, qty_dec)) for p, q in rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--price-dec", type=int, default=2)
    ap.add_argument("--qty-dec", type=int, default=5)
    a = ap.parse_args()
    n = {"diff": 0, "trade": 0, "snap": 0}
    with open(a.src) as f, open(a.dst, "wb") as out:
        out.write(b"EXMD" + struct.pack("<III", 1, a.price_dec, a.qty_dec))
        for line in f:
            r = json.loads(line)
            k = r["k"]
            if k == "diff":
                body = struct.pack("<QQII", r["U"], r["u"], len(r["b"]), len(r["a"]))
                body += levels(r["b"], a.price_dec, a.qty_dec) + levels(r["a"], a.price_dec, a.qty_dec)
                kind = 1
            elif k == "trade":
                body = struct.pack("<qQB7x", scaled(r["p"], a.price_dec), scaled(r["q"], a.qty_dec), 1 if r["m"] else 0)
                kind = 2
            elif k == "snap":
                body = struct.pack("<QII", r["id"], len(r["b"]), len(r["a"]))
                body += levels(r["b"], a.price_dec, a.qty_dec) + levels(r["a"], a.price_dec, a.qty_dec)
                kind = 3
            else:
                continue
            payload = struct.pack("<B3xQ", kind, r["rx"]) + body
            out.write(struct.pack("<I", len(payload)) + payload)
            n[k] += 1
    print(f"converted {n} -> {a.dst}")


if __name__ == "__main__":
    main()
