// exsim_bench_liquibook: head-to-head against Liquibook, an established open-source C++ matching engine.
//
// Both engines process the IDENTICAL command stream, restricted to semantics they share (Day and IOC
// limit orders plus cancels; see WorkloadConfig::simple). Correctness comes first: before any timing,
// the two engines must agree on the number of trades and the total traded volume. Timing then uses
// interleaved repetitions, and the harness reports the distribution, not just the best run.
//
// Liquibook is fetched by scripts/fetch_liquibook.sh and enabled with -DEXSIM_LIQUIBOOK_DIR=<path>.
// Its book is std::multimap-based and reports through a callback vector flushed after every
// operation. That is a fair picture of a conventional design, and exactly the design this project
// argues against for latency-critical use.
//
//   exsim_bench_liquibook [--messages 3000000] [--symbols 8] [--reps 9] [--cpu 2]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <book/order_book.h>
#include <simple/simple_order.h>

#include "../tools/args.hpp"
#include "stats.hpp"
#include "exsim/cpu.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/workload.hpp"

using namespace exsim;

namespace {

struct Totals {
  std::uint64_t trades = 0, volume = 0;
};

// Minimal sink for our engine: count trades and volume, nothing else.
struct OurSink {
  Totals t;
  EXSIM_ALWAYS_INLINE void on_event(const Event& e) noexcept {
    if (e.type == EventType::Trade) ++t.trades, t.volume += e.qty;
  }
};

using LbOrder = liquibook::simple::SimpleOrder*;
using LbBook = liquibook::book::OrderBook<LbOrder>;

struct LbListener : liquibook::book::OrderListener<LbOrder>, liquibook::book::TradeListener<LbBook> {
  Totals t;
  void on_accept(const LbOrder&) override {}
  void on_reject(const LbOrder&, const char*) override {}
  void on_fill(const LbOrder&, const LbOrder&, liquibook::book::Quantity, liquibook::book::Price) override {}
  void on_cancel(const LbOrder&) override {}
  void on_cancel_reject(const LbOrder&, const char*) override {}
  void on_replace(const LbOrder&, const int64_t&, liquibook::book::Price) override {}
  void on_replace_reject(const LbOrder&, const char*) override {}
  void on_trade(const LbBook*, liquibook::book::Quantity qty, liquibook::book::Price) override { ++t.trades, t.volume += qty; }
};

Totals run_ours(const std::vector<Command>& cmds, std::uint32_t symbols, double* secs) {
  MatchingEngine<DefaultBook> engine(symbols, BookConfig{});
  OurSink sink;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Command& c : cmds) engine.process(c, sink);
  *secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return sink.t;
}

// Orders are constructed before the clock starts: object creation is the caller's cost in Liquibook.
struct LbRun {
  std::vector<liquibook::simple::SimpleOrder> orders;  // orders[id - 1]
  std::vector<std::unique_ptr<LbBook>> books;
  LbListener listener;

  LbRun(const std::vector<Command>& cmds, std::uint32_t symbols) {
    std::size_t n = 0;
    for (const Command& c : cmds) n += c.type == MsgType::NewOrder;
    orders.reserve(n);
    for (const Command& c : cmds) {
      if (c.type != MsgType::NewOrder) continue;
      const bool ioc = c.tif == Tif::Ioc;
      orders.emplace_back(c.side == Side::Buy, static_cast<liquibook::book::Price>(c.price), c.qty, 0,
                          ioc ? liquibook::book::oc_immediate_or_cancel : liquibook::book::oc_no_conditions);
    }
    for (std::uint32_t i = 0; i < symbols; ++i) {
      books.push_back(std::make_unique<LbBook>("S"));
      books.back()->set_order_listener(&listener);
      books.back()->set_trade_listener(&listener);
    }
  }

  Totals run(const std::vector<Command>& cmds, double* secs) {
    const auto t0 = std::chrono::steady_clock::now();
    for (const Command& c : cmds) {
      LbBook& b = *books[c.symbol];
      LbOrder o = &orders[c.order_id - 1];
      if (c.type == MsgType::NewOrder) b.add(o, o->immediate_or_cancel() ? liquibook::book::oc_immediate_or_cancel : 0);
      else b.cancel(o);
    }
    *secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return listener.t;
  }
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  WorkloadConfig wc;
  wc.messages = args.u64("messages", 3'000'000);
  wc.symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  wc.book.max_orders = static_cast<std::uint32_t>(args.u64("max-orders", 1u << 16));  // sized for this workload's peak
  wc.simple = true;
  const int reps = static_cast<int>(args.u64("reps", 9));
  pin_current_thread(static_cast<int>(args.i64("cpu", 2)));

  const auto cmds = generate_workload(wc);
  std::printf("workload: %zu commands, %u symbols, shared semantics only (Day/IOC limits + cancels)\n", cmds.size(), wc.symbols);

  // ---- correctness gate ----
  double s;
  const Totals a = run_ours(cmds, wc.symbols, &s);
  const Totals b = LbRun(cmds, wc.symbols).run(cmds, &s);
  std::printf("agreement check: exsim %llu trades / %llu lots; Liquibook %llu trades / %llu lots -> %s\n",
              static_cast<unsigned long long>(a.trades), static_cast<unsigned long long>(a.volume),
              static_cast<unsigned long long>(b.trades), static_cast<unsigned long long>(b.volume),
              a.trades == b.trades && a.volume == b.volume ? "IDENTICAL" : "MISMATCH");
  if (a.trades != b.trades || a.volume != b.volume) {
    std::printf("engines disagree; timing would be meaningless. Aborting.\n");
    return 1;
  }

  // ---- timing, interleaved ----
  std::vector<double> ours, lb;
  for (int r = 0; r < reps; ++r) {
    run_ours(cmds, wc.symbols, &s), ours.push_back(static_cast<double>(cmds.size()) / s / 1e6);
    LbRun run(cmds, wc.symbols);
    run.run(cmds, &s), lb.push_back(static_cast<double>(cmds.size()) / s / 1e6);
  }
  auto best = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); };
  auto worst = [](const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); };
  std::printf("\nM msg/s over %d interleaved reps        best     median      worst\n", reps);
  std::printf("  exsim (aos, locality index)     %9.2f  %9.2f  %9.2f\n", best(ours), median(ours), worst(ours));
  std::printf("  Liquibook (SimpleOrder)         %9.2f  %9.2f  %9.2f\n", best(lb), median(lb), worst(lb));
  std::printf("  ratio                           %8.2fx  %8.2fx  %8.2fx\n", best(ours) / best(lb), median(ours) / median(lb),
              worst(ours) / worst(lb));
  const auto ci = bench::bootstrap_median_ci(bench::paired_ratios(ours, lb));
  std::printf("\nspeedup over Liquibook, median of %d paired interleaved reps: %.1fx  (95%% bootstrap CI %.1fx to %.1fx)\n",
              reps, ci[0], ci[1], ci[2]);
  return 0;
}
