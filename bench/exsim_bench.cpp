// exsim_bench: single-threaded matching throughput and latency for every design point, plus the SPSC
// ring in isolation.
//
// Design points (see BENCHMARKS.md):
//   ref          std::map<price, std::list<Order>> + std::unordered_map index  (textbook baseline)
//   aos-scatter  dense price ladder + bitmap + slab pool + flat index (fmix64 hash), 48 B AoS records
//   aos          same, locality-preserving index hash
//   soa          same, pure structure-of-arrays order store
//   hybrid       same, hot/cold split order store (16 B hot record)         (shipped default)
//
// Method: one deterministic workload is generated up front and replayed through a fresh engine for
// every repetition; repetitions are interleaved round-robin across implementations. Throughput is timed over the whole replay with no per-message instrumentation.
// A separate pass brackets each message with serialized rdtsc to build a latency histogram per
// message type. A global operator new hook counts heap allocations inside the timed region.
//
//   exsim_bench [--messages 5000000] [--symbols 8] [--seed 42] [--reps 7] [--impl ref,aos-scatter,aos,soa,hybrid]
//               [--cpu 2] [--no-latency] [--spsc] [--spsc-items 50000000] [--max-orders N] [--levels N]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>  // lets scripts/cache_profile.sh measure only the matching loop
#define EXSIM_CG_START() CACHEGRIND_START_INSTRUMENTATION
#define EXSIM_CG_STOP() CACHEGRIND_STOP_INSTRUMENTATION
#else
#define EXSIM_CG_START() ((void)0)
#define EXSIM_CG_STOP() ((void)0)
#endif

#include "../tools/args.hpp"
#include "exsim/clock.hpp"
#include "exsim/cpu.hpp"
#include "exsim/latency_histogram.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"
#include "exsim/spsc_queue.hpp"
#include "exsim/workload.hpp"

// ---- allocation counter ----------------------------------------------------------------------
static std::atomic<std::uint64_t> g_allocs{0};
// GCC's -Wmismatched-new-delete cannot see that these replacements pair malloc with free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
#if !EXSIM_TSAN_BUILD
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace exsim;

struct Result {
  std::string name;
  std::vector<double> mps;  // messages per second, one per rep
  std::uint64_t digest = 0;
  std::uint64_t allocs = 0;
  std::uint64_t trades = 0;
  LatencyHistogram lat_all, lat_new, lat_cancel, lat_modify;
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}

// One throughput repetition: fresh engine (construction untimed), full replay, no instrumentation.
template <class Book>
void throughput_rep(Result& r, const std::vector<Command>& cmds, std::uint32_t symbols, const BookConfig& cfg) {
  MatchingEngine<Book> engine(symbols, cfg);
  CountingSink sink;
  const std::uint64_t a0 = g_allocs.load();
  EXSIM_CG_START();
  const auto t0 = std::chrono::steady_clock::now();
  for (const Command& c : cmds) engine.process(c, sink);
  const auto t1 = std::chrono::steady_clock::now();
  EXSIM_CG_STOP();
  r.allocs = std::max(r.allocs, g_allocs.load() - a0);
  r.mps.push_back(static_cast<double>(cmds.size()) / std::chrono::duration<double>(t1 - t0).count());
  if (r.digest != 0 && r.digest != sink.digest()) {
    std::fprintf(stderr, "FATAL: %s digest differs between runs\n", r.name.c_str());
    std::exit(1);
  }
  r.digest = sink.digest();
  r.trades = sink.count(EventType::Trade);
}

// One latency repetition: every message bracketed by serialized TSC reads; histograms accumulate.
template <class Book>
void latency_rep(Result& r, const std::vector<Command>& cmds, std::uint32_t symbols, const BookConfig& cfg) {
  MatchingEngine<Book> engine(symbols, cfg);
  CountingSink sink;
  for (const Command& c : cmds) {
    const std::uint64_t t0 = tsc_begin();
    engine.process(c, sink);
    const std::uint64_t dt = tsc_end() - t0;
    r.lat_all.record(dt);
    switch (c.type) {
      case MsgType::NewOrder: r.lat_new.record(dt); break;
      case MsgType::Cancel: r.lat_cancel.record(dt); break;
      case MsgType::Modify: r.lat_modify.record(dt); break;
    }
  }
}

struct Runner {
  Result result;
  void (*throughput)(Result&, const std::vector<Command>&, std::uint32_t, const BookConfig&);
  void (*latency)(Result&, const std::vector<Command>&, std::uint32_t, const BookConfig&);
};

template <class Book>
Runner make_runner(const char* name) {
  Runner r{{}, &throughput_rep<Book>, &latency_rep<Book>};
  r.result.name = name;
  return r;
}

std::uint64_t timer_overhead_ticks() {
  LatencyHistogram h;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t t0 = tsc_begin();
    h.record(tsc_end() - t0);
  }
  return h.percentile(50);
}

void bench_spsc(std::uint64_t items, int cpu_a, int cpu_b) {
  SpscQueue<Command> q(1 << 16);
  std::atomic<bool> go{false};
  std::uint64_t checksum = 0;
  std::thread consumer([&] {
    pin_current_thread(cpu_b);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::uint64_t i = 0; i < items;) {
      if (Command* c = q.front()) {
        checksum += c->order_id;
        q.pop();
        ++i;
      } else {
        cpu_relax();
      }
    }
  });
  pin_current_thread(cpu_a);
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  Command c{};
  for (std::uint64_t i = 0; i < items; ++i) {
    c.order_id = i;
    while (!q.try_push(c)) cpu_relax();
  }
  consumer.join();
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const bool ok = checksum == items * (items - 1) / 2;
  std::printf("\nSPSC ring, %zu-byte Command, 2 threads (cpu %d -> cpu %d): %.1f M items/s  [%s]\n", sizeof(Command),
              cpu_a, cpu_b, static_cast<double>(items) / s / 1e6, ok ? "checksum ok" : "CHECKSUM MISMATCH");
}

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  WorkloadConfig wc;
  wc.messages = args.u64("messages", 5'000'000);
  wc.symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  wc.seed = args.u64("seed", 42);
  wc.target_live = static_cast<std::uint32_t>(args.u64("target-live", 4096));
  const int reps = static_cast<int>(args.u64("reps", 7));
  const std::string impls = "," + args.str("impl", "ref,aos-scatter,aos,soa,hybrid") + ",";
  const bool latency = !args.has("no-latency");
  const int cpu = static_cast<int>(args.i64("cpu", 2));

  const bool pinned = pin_current_thread(cpu);
  const double ns_per_tick = calibrate_ns_per_tick();
  std::printf("exsim_bench: %llu messages, %u symbols, seed %llu, %d reps, pinned to cpu %d: %s\n",
              static_cast<unsigned long long>(wc.messages), wc.symbols, static_cast<unsigned long long>(wc.seed), reps,
              cpu, pinned ? "yes" : "no");

  WorkloadMix mix;
  const auto cmds = generate_workload(wc, &mix);
  const double n = static_cast<double>(cmds.size());
  std::printf("workload: new %.1f%% (marketable %.1f%%, ioc/fok/market %.1f%%), cancel %.1f%%, modify %.1f%%\n",
              100.0 * static_cast<double>(mix.new_orders) / n, 100.0 * static_cast<double>(mix.marketable) / n,
              100.0 * static_cast<double>(mix.ioc_fok_market) / n, 100.0 * static_cast<double>(mix.cancels) / n,
              100.0 * static_cast<double>(mix.modifies) / n);
  std::printf("TSC: %.4f ns/tick; timer overhead (lfence;rdtsc..rdtscp;lfence) p50 = %.1f ns (not subtracted)\n\n",
              ns_per_tick, static_cast<double>(timer_overhead_ticks()) * ns_per_tick);

  BookConfig cfg{};
  cfg.max_orders = static_cast<std::uint32_t>(args.u64("max-orders", cfg.max_orders));
  cfg.num_levels = static_cast<std::uint32_t>(args.u64("levels", cfg.num_levels));
  std::printf("book config: %u price levels, %u max orders per symbol\n\n", cfg.num_levels, cfg.max_orders);

  std::vector<Runner> runners;
  if (impls.find(",ref,") != std::string::npos) runners.push_back(make_runner<ReferenceBook>("ref"));
  if (impls.find(",aos-scatter,") != std::string::npos)
    runners.push_back(make_runner<AosScatterBook>("aos-scatter"));
  if (impls.find(",aos,") != std::string::npos) runners.push_back(make_runner<AosBook>("aos"));
  if (impls.find(",soa,") != std::string::npos) runners.push_back(make_runner<SoaBook>("soa"));
  if (impls.find(",hybrid,") != std::string::npos) runners.push_back(make_runner<HybridBook>("hybrid"));

  // Repetitions are interleaved round-robin across implementations, so slow drift (thermal
  // throttling, turbo budget, background load) is spread evenly instead of penalizing whichever
  // implementation happened to run during a bad stretch. Noise only ever adds time, so best-of-N is
  // the headline figure; the median is reported alongside it.
  for (int rep = 0; rep < reps; ++rep)
    for (Runner& r : runners) r.throughput(r.result, cmds, wc.symbols, cfg);
  if (latency)
    for (int rep = 0; rep < std::max(1, reps / 3); ++rep)
      for (Runner& r : runners) r.latency(r.result, cmds, wc.symbols, cfg);
  std::vector<Result> results;
  for (Runner& r : runners) results.push_back(std::move(r.result));

  auto ns = [&](std::uint64_t t) { return static_cast<double>(t) * ns_per_tick; };
  auto best = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); };
  std::printf("| impl        | M msg/s (best) | M msg/s (median) | ns/msg (best) | best vs ref | heap allocs in loop | digest           |\n");
  std::printf("|-------------|---------------:|-----------------:|--------------:|------------:|--------------------:|------------------|\n");
  const double ref_best = results.empty() || results[0].name != "ref" ? 0 : best(results[0].mps);
  for (const auto& r : results) {
    const double b = best(r.mps);
    std::printf("| %-11s | %14.2f | %16.2f | %13.1f | %10.2fx | %19llu | %016llx |\n", r.name.c_str(), b / 1e6,
                median(r.mps) / 1e6, 1e9 / b, ref_best > 0 ? b / ref_best : 0.0,
                static_cast<unsigned long long>(r.allocs), static_cast<unsigned long long>(r.digest));
  }
  bool same = true;
  for (const auto& r : results) same &= r.digest == results[0].digest;
  std::printf("\nevent streams identical across implementations: %s (%llu trades)\n", same ? "YES" : "NO -- BUG",
              results.empty() ? 0ull : static_cast<unsigned long long>(results[0].trades));

  if (latency) {
    std::printf("\nper-message latency, ns (serialized rdtsc around engine.process)\n");
    std::printf("| impl        | type   |   p50 |   p90 |   p99 | p99.9 | p99.99 |     max |  mean |\n");
    std::printf("|-------------|--------|------:|------:|------:|------:|-------:|--------:|------:|\n");
    for (const auto& r : results) {
      const std::pair<const char*, const LatencyHistogram*> rows[] = {
          {"all", &r.lat_all}, {"new", &r.lat_new}, {"cancel", &r.lat_cancel}, {"modify", &r.lat_modify}};
      for (const auto& [t, h] : rows)
        std::printf("| %-11s | %-6s | %5.0f | %5.0f | %5.0f | %5.0f | %6.0f | %7.0f | %5.1f |\n", r.name.c_str(), t,
                    ns(h->percentile(50)), ns(h->percentile(90)), ns(h->percentile(99)), ns(h->percentile(99.9)),
                    ns(h->percentile(99.99)), ns(h->max()), h->mean() * ns_per_tick);
    }
  }

  if (args.has("spsc")) bench_spsc(args.u64("spsc-items", 50'000'000), cpu, static_cast<int>(args.i64("cpu2", cpu + 2)));
  return same ? 0 : 1;
}
