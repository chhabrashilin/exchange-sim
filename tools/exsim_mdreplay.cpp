// exsim_mdreplay: replays a real exchange capture through the matching engine and validates the
// reconstructed book against the exchange's own REST snapshots.
//
// Validation is exact, not approximate. A snapshot carries `lastUpdateId`, the id of the last update
// reflected in it. When our applied diff sequence reaches a diff whose last id EQUALS that value,
// our book must equal the exchange's book at that instant, level for level (price and quantity), for
// the top N levels of each side. Snapshots whose id falls in the middle of a diff cannot be checked
// exactly (the exchange does not publish its intermediate states) and are counted separately, not
// silently ignored.
//
// --inject-drop N discards every Nth level update: a mutation test proving the check can fail.
//
//   exsim_mdreplay --in btcusdt.exmd [--top 400] [--inject-drop 0] [--verbose] [--diagnose N]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <unordered_map>

#include "args.hpp"
#include "exsim/clock.hpp"
#include "exsim/latency_histogram.hpp"
#include "exsim/md.hpp"
#include "exsim/md_mirror.hpp"

using namespace exsim;

namespace {

std::int64_t mid_of(const md::Record& snap) {
  return (snap.bid(0).px + snap.ask(0).px) / 2;
}

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (!args.has("in")) tools::Args::die("--in <file.exmd> is required");
  const auto top_n = static_cast<std::size_t>(args.u64("top", 400));
  const std::uint64_t drop = args.u64("inject-drop", 0);
  const bool verbose = args.has("verbose");
  const std::uint64_t diagnose = args.u64("diagnose", 0);  // print this many mismatching levels of the first failing snapshot
  std::uint64_t diag_printed = 0;

  md::Capture cap(args.str("in", ""));
  const auto& recs = cap.records();
  std::vector<std::size_t> diffs, snaps;
  std::size_t trades = 0;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].kind == md::Kind::Diff) diffs.push_back(i);
    else if (recs[i].kind == md::Kind::Snapshot) snaps.push_back(i);
    else ++trades;
  }
  std::printf("capture: %zu records (%zu diffs, %zu trades, %zu snapshots), price x1e-%u, qty x1e-%u\n", recs.size(),
              diffs.size(), trades, snaps.size(), cap.price_decimals(), cap.qty_decimals());
  if (diffs.empty() || snaps.empty()) tools::Args::die("need at least one diff and one snapshot");
  for (std::size_t i = 1; i < diffs.size(); ++i)
    if (recs[diffs[i]].first_id < recs[diffs[i - 1]].first_id) tools::Args::die("diff ids are not monotonic");

  // snapshots by lastUpdateId (candidates for exact validation)
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> by_id;
  for (std::size_t s : snaps) by_id[recs[s].last_id].push_back(s);

  md::Seed seed{};
  if (!md::find_seed(recs, diffs, snaps, 0, seed)) tools::Args::die("no snapshot is straddled by the diff stream");
  const md::Record& first_snap = recs[seed.snap_rec];
  BookMirror mirror(mid_of(first_snap));
  mirror.seed(first_snap);
  std::printf("seeded from snapshot id %llu (%u bids, %u asks)\n", static_cast<unsigned long long>(first_snap.last_id),
              first_snap.n_bids, first_snap.n_asks);

  const double ns_per_tick = calibrate_ns_per_tick();
  LatencyHistogram apply_lat;
  std::uint64_t applied = 0, skipped_pre = 0, gaps = 0, resyncs = 0, reseeds = 0;
  std::uint64_t exact_checked = 0, exact_pass = 0, exact_levels = 0, mismatched_levels = 0;
  std::uint64_t prev_last = 0;
  bool have_prev = false;

  // apply diffs starting at the seed diff, in file order
  std::size_t dpos = static_cast<std::size_t>(std::find(diffs.begin(), diffs.end(), seed.diff_rec) - diffs.begin());
  skipped_pre = dpos;
  for (; dpos < diffs.size(); ++dpos) {
    const md::Record& df = recs[diffs[dpos]];
    if (have_prev && df.first_id != prev_last + 1) {
      ++gaps;
      md::Seed next{};
      if (!md::find_seed(recs, diffs, snaps, dpos, next)) {
        std::printf("sequence gap at diff %zu and no snapshot to resync from; stopping\n", dpos);
        break;
      }
      const md::Record& s = recs[next.snap_rec];
      mirror.seed(s);
      ++resyncs;
      dpos = static_cast<std::size_t>(std::find(diffs.begin(), diffs.end(), next.diff_rec) - diffs.begin());
      have_prev = false;
      // fallthrough to apply the resync diff, re-reading it
      const md::Record& rd = recs[diffs[dpos]];
      mirror.apply(rd, drop);
      prev_last = rd.last_id;
      have_prev = true;
      ++applied;
      continue;
    }
    const std::uint64_t t0 = tsc_begin();
    mirror.apply(df, drop);
    apply_lat.record(tsc_end() - t0);
    prev_last = df.last_id;
    have_prev = true;
    ++applied;

    if (auto it = by_id.find(df.last_id); it != by_id.end()) {
      for (std::size_t s : it->second) {
        const md::Record& snap = recs[s];
        // Compare only inside the region the mirror is provably complete for (see md_mirror.hpp): levels
        // deeper than the seed snapshot that never change are invisible to a diff-based reconstruction.
        const auto mb = mirror.top(Side::Buy, top_n, true), ma = mirror.top(Side::Sell, top_n, true);
        std::uint64_t bad = 0, checked = 0;
        auto cmp = [&](const std::vector<md::LevelUpdate>& ours, bool bids) {
          std::vector<md::LevelUpdate> theirs;
          const std::uint32_t total = bids ? snap.n_bids : snap.n_asks;
          for (std::uint32_t i = 0; i < total && theirs.size() < top_n; ++i) {
            const md::LevelUpdate l = bids ? snap.bid(i) : snap.ask(i);
            if (bids ? l.px < mirror.bid_horizon() : l.px > mirror.ask_horizon()) break;
            theirs.push_back(l);
          }
          const std::size_t n = std::max(theirs.size(), ours.size());  // a missing or extra level is a mismatch
          for (std::size_t i = 0; i < n; ++i) {
            ++checked;
            const bool differs = i >= ours.size() || i >= theirs.size() || ours[i].px != theirs[i].px || ours[i].qty != theirs[i].qty;
            if (!differs) continue;
            ++bad;
            if (diagnose && diag_printed < diagnose) {
              ++diag_printed;
              std::printf("    %s level %zu: exchange %lld x %llu, ours %lld x %llu\n", bids ? "bid" : "ask", i,
                          i < theirs.size() ? static_cast<long long>(theirs[i].px) : -1LL,
                          i < theirs.size() ? static_cast<unsigned long long>(theirs[i].qty) : 0ULL,
                          i < ours.size() ? static_cast<long long>(ours[i].px) : -1LL,
                          i < ours.size() ? static_cast<unsigned long long>(ours[i].qty) : 0ULL);
            }
          }
        };
        cmp(mb, true);
        cmp(ma, false);
        ++exact_checked;
        exact_levels += checked;
        mismatched_levels += bad;
        if (bad == 0) ++exact_pass;
        if (verbose || bad != 0)
          std::printf("  snapshot %llu: best bid %lld / ask %lld, %llu levels, %llu mismatches -> %s\n",
                      static_cast<unsigned long long>(snap.last_id), static_cast<long long>(mb.empty() ? 0 : mb[0].px),
                      static_cast<long long>(ma.empty() ? 0 : ma[0].px), static_cast<unsigned long long>(checked),
                      static_cast<unsigned long long>(bad), bad == 0 ? "PASS" : "FAIL");
        // The market has consumed much of the seeded depth: reseed, exactly, from this aligned snapshot.
        if (mirror.needs_reseed()) {
          mirror.seed(snap);
          ++reseeds;
        }
      }
    }
  }

  // classify the remaining snapshots inside the replayed id range
  std::uint64_t in_range = 0, unaligned = 0;
  const std::uint64_t lo = recs[seed.diff_rec].first_id, hi = prev_last;
  for (std::size_t s : snaps) {
    const std::uint64_t id = recs[s].last_id;
    if (id < lo || id > hi) continue;
    ++in_range;
    bool aligned = false;
    for (std::size_t d : diffs) if (recs[d].last_id == id) { aligned = true; break; }
    if (!aligned) ++unaligned;
  }

  auto ns = [&](std::uint64_t t) { return static_cast<double>(t) * ns_per_tick; };
  std::printf("\nreplay: %llu diffs applied (%llu before the seed skipped), %llu sequence gaps, %llu resyncs, %llu depth reseeds\n",
              static_cast<unsigned long long>(applied), static_cast<unsigned long long>(skipped_pre),
              static_cast<unsigned long long>(gaps), static_cast<unsigned long long>(resyncs), static_cast<unsigned long long>(reseeds));
  std::printf("mirror: %llu level updates into the engine, %llu out of band, %llu book-full, %llu rejects\n",
              static_cast<unsigned long long>(mirror.level_updates()), static_cast<unsigned long long>(mirror.out_of_band()),
              static_cast<unsigned long long>(mirror.book_full()), static_cast<unsigned long long>(mirror.rejects()));
  std::printf("phantom trades (must be 0): %llu\n", static_cast<unsigned long long>(mirror.phantom_trades()));
  std::printf("diff apply latency: p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns, max %.0f ns\n", ns(apply_lat.percentile(50)),
              ns(apply_lat.percentile(99)), ns(apply_lat.percentile(99.9)), ns(apply_lat.max()));
  std::printf("snapshots in the replayed id range: %llu; exactly aligned to a diff boundary: %llu; mid-diff (not exactly checkable): %llu\n",
              static_cast<unsigned long long>(in_range), static_cast<unsigned long long>(in_range - unaligned),
              static_cast<unsigned long long>(unaligned));
  std::printf("exact validation (up to top %zu levels/side inside the complete region, price and qty): %llu/%llu snapshots exact, %llu levels compared, %llu mismatched\n",
              top_n, static_cast<unsigned long long>(exact_pass), static_cast<unsigned long long>(exact_checked),
              static_cast<unsigned long long>(exact_levels), static_cast<unsigned long long>(mismatched_levels));
  const bool ok = exact_checked > 0 && exact_pass == exact_checked && mirror.phantom_trades() == 0;
  std::printf("%s\n", drop != 0 ? (ok ? "fault injection NOT detected (few drops, possibly all deeper than the compared levels)" : "fault injection detected, as it should be")
                                : (ok ? "BOOK RECONSTRUCTION VALIDATED" : "VALIDATION FAILED"));
  return drop != 0 ? (ok ? 1 : 0) : (ok ? 0 : 1);
}
