#!/usr/bin/env python3
"""Estimate the Avellaneda-Stoikov order-arrival parameter k from real trades.

A-S assumes the rate at which a quote at distance d from the mid gets hit is  lambda(d) = A * exp(-k d).
A quote at distance d is reached by every trade that hits a resting order at distance >= d from the mid,
so lambda(d) is the empirical survival rate of trade depth.

    exsim_mmsim --in data.exmd --calibrate depth.csv
    python calibrate_k.py depth.csv [--plot out.png]

Prints k (per tick), A (per second), the fit quality, and a power-law alternative. If the exponential
fits poorly, that is reported: trade depth in crypto order books is heavy-tailed, and A-S's exponential
assumption is then only a local approximation.
"""
import argparse, json
import numpy as np
import pandas as pd


def fit_line(x, y):
    A = np.vstack([x, np.ones_like(x)]).T
    (m, c), res, *_ = np.linalg.lstsq(A, y, rcond=None)
    pred = m * x + c
    ss_res = float(np.sum((y - pred) ** 2))
    ss_tot = float(np.sum((y - y.mean()) ** 2))
    return m, c, 1 - ss_res / ss_tot if ss_tot > 0 else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--plot")
    ap.add_argument("--min-count", type=int, default=50, help="ignore depths reached by fewer trades than this")
    ap.add_argument("--dmax", type=float, default=400, help="fit only distances up to this many ticks")
    a = ap.parse_args()
    df = pd.read_csv(a.csv)
    T = float(df.t_s.max() - df.t_s.min())
    d = np.sort(df.dist_ticks.to_numpy())
    grid = np.unique(np.round(np.geomspace(1, max(2.0, d.max()), 60)))
    counts = np.array([(d >= g).sum() for g in grid])
    keep = (counts >= a.min_count) & (grid <= a.dmax) & (grid >= 1)
    g, n = grid[keep], counts[keep]
    lam = n / T
    k_exp, lnA, r2_exp = fit_line(g, np.log(lam))
    p_pow, lnB, r2_pow = fit_line(np.log(g), np.log(lam))
    res = {
        "trades": int(len(df)), "seconds": round(T, 1), "median_spread_ticks": float(df.spread_ticks.median()),
        "median_trade_depth_ticks": float(np.median(d)), "p90_trade_depth_ticks": float(np.percentile(d, 90)),
        "k_per_tick": float(-k_exp), "A_per_sec": float(np.exp(lnA)), "r2_exponential": float(r2_exp),
        "power_law_exponent": float(-p_pow), "r2_power_law": float(r2_pow), "fit_range_ticks": [float(g.min()), float(g.max())],
    }
    print(json.dumps(res, indent=2))
    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(6.4, 4.2))
        ax.loglog(g, lam, "o", ms=4, label="measured")
        ax.loglog(g, np.exp(lnA) * np.exp(k_exp * g), "-", label=f"exponential  k={-k_exp:.4f}/tick  R2={r2_exp:.2f}")
        ax.loglog(g, np.exp(lnB) * g ** p_pow, "--", label=f"power law  alpha={-p_pow:.2f}  R2={r2_pow:.2f}")
        ax.set_xlabel("distance from mid (ticks)")
        ax.set_ylabel("trades per second reaching that depth")
        ax.set_title("Fill intensity vs quote distance")
        ax.legend()
        fig.tight_layout()
        fig.savefig(a.plot, dpi=140)


if __name__ == "__main__":
    main()
