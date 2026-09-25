// exsim_pipeline: the exchange as three pinned threads joined by lock-free SPSC rings.
//
//   feed thread         -> [SpscQueue<Command>] -> engine thread -> [SpscQueue<MdMsg>] -> md publisher thread
//   decode binary wire                              match, emit trades                    consume trades
//   stamp ingress TSC                               and top-of-book updates               + BBO updates
//
// Reports sustained throughput and two latency distributions:
//   ingress -> match complete   (decode + queue transit + matching)
//   ingress -> md published     (the above + event fan-out + second queue hop)
//
// With --rate N the feed paces itself to N msg/s, and each message's ingress stamp is its *scheduled*
// send time, not the time it actually went out. If the pipeline falls behind, the delay shows up in
// the latency numbers instead of being hidden (this corrects for "coordinated omission"). Without
// --rate the feed runs flat out, which measures saturation throughput; queueing delay then dominates
// the latency numbers.
//
// The engine thread's event digest is printed and must equal exsim_replay's digest for the same
// capture: threading must not change results.
//
//   exsim_pipeline --in flow.bin [--rate 2000000] [--feed-cpu 2 --engine-cpu 4 --md-cpu 6]
//                  [--queue 65536] [--warmup 200000] [--symbols 8] [--prefetch 8 (0 = off)]
//   exsim_pipeline --jitter 5 [--engine-cpu 4]      # hiccup meter: how much does the OS steal?

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "args.hpp"
#include "exsim/clock.hpp"
#include "exsim/cpu.hpp"
#include "exsim/latency_histogram.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"
#include "exsim/spsc_queue.hpp"
#include "exsim/wire_file.hpp"

namespace {

using namespace exsim;

enum class MdKind : std::uint8_t { Trade = 1, Bbo = 2 };

struct MdMsg {
  std::uint64_t ingress_ts;
  Price px_a;  // Trade: price;       Bbo: bid price (INT64_MIN if none)
  Price px_b;  //                     Bbo: ask price (INT64_MIN if none)
  std::uint64_t qty_a;  // Trade: qty;  Bbo: bid qty
  std::uint64_t qty_b;  //              Bbo: ask qty
  SymbolId symbol;
  MdKind kind;
  Side side;
  std::uint16_t pad;
};
static_assert(sizeof(MdMsg) == 48);

struct EngineSink {
  SpscQueue<MdMsg>& md;
  std::uint64_t ingress = 0;
  std::uint64_t md_stalls = 0;
  CountingSink counts;

  EXSIM_ALWAYS_INLINE void publish(const MdMsg& m) noexcept {
    while (!md.try_push(m)) {
      ++md_stalls;
      cpu_relax();
    }
  }
  EXSIM_ALWAYS_INLINE void on_event(const Event& e) noexcept {
    counts.on_event(e);
    if (e.type == EventType::Trade)
      publish(MdMsg{ingress, e.price, 0, e.qty, 0, e.symbol, MdKind::Trade, e.side, 0});
  }
};

struct Tob {
  Price bid = INT64_MIN, ask = INT64_MIN;
  std::uint64_t bid_qty = 0, ask_qty = 0;
  bool operator==(const Tob&) const = default;
};

// Cross-core latency: the stamp was taken on another core. Under virtualization (WSL2, cloud VMs)
// per-vCPU TSCs can disagree by a little, so a delta can come out negative. Unsigned subtraction
// would turn that into a ~2^64 "latency". Record such samples as 0 and count them, so the skew is
// reported instead of silently corrupting the tail.
struct CrossCoreLatency {
  LatencyHistogram hist;
  std::uint64_t negative = 0;
  EXSIM_ALWAYS_INLINE void record(std::uint64_t now, std::uint64_t stamp) noexcept {
    const auto d = static_cast<std::int64_t>(now - stamp);
    if (EXSIM_UNLIKELY(d < 0)) ++negative;
    hist.record(d < 0 ? 0 : static_cast<std::uint64_t>(d));
  }
};

void print_hist(const char* name, const LatencyHistogram& h, double ns_per_tick) {
  auto ns = [&](std::uint64_t t) { return static_cast<double>(t) * ns_per_tick; };
  std::printf("  %-28s n=%-10llu p50 %7.0f  p90 %7.0f  p99 %7.0f  p99.9 %8.0f  p99.99 %8.0f  max %9.0f ns\n", name,
              static_cast<unsigned long long>(h.count()), ns(h.percentile(50)), ns(h.percentile(90)),
              ns(h.percentile(99)), ns(h.percentile(99.9)), ns(h.percentile(99.99)), ns(h.max()));
}

}  // namespace

// Hiccup meter: spin on a pinned core reading the TSC back-to-back. Any gap between consecutive
// reads is time the core was taken away (interrupts, SMIs, hypervisor descheduling). This shows
// the floor under which no user-space latency number on this machine can go.
int run_jitter_probe(double seconds, int cpu) {
  const bool pinned = pin_current_thread(cpu);
  const double ns_per_tick = calibrate_ns_per_tick();
  LatencyHistogram gaps;
  std::uint64_t stolen = 0, over_10us = 0;
  const auto limit = static_cast<std::uint64_t>(seconds * 1e9 / ns_per_tick);
  const std::uint64_t start = rdtsc();
  std::uint64_t prev = start;
  while (prev - start < limit) {
    const std::uint64_t now = rdtsc();
    const std::uint64_t gap = now - prev;
    gaps.record(gap);
    if (static_cast<double>(gap) * ns_per_tick > 1000.0) stolen += gap;
    if (static_cast<double>(gap) * ns_per_tick > 10000.0) ++over_10us;
    prev = now;
  }
  const double total = static_cast<double>(prev - start);
  std::printf("jitter probe: %.1f s on cpu %d (pinned: %s)\n", seconds, cpu, pinned ? "yes" : "no");
  print_hist("TSC read-to-read gap", gaps, ns_per_tick);
  std::printf("  time lost to gaps > 1 us: %.3f%%; gaps > 10 us: %llu (%.1f per second)\n",
              100.0 * static_cast<double>(stolen) / total, static_cast<unsigned long long>(over_10us),
              static_cast<double>(over_10us) / seconds);
  return 0;
}

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (args.has("jitter")) return run_jitter_probe(args.f64("jitter", 5), static_cast<int>(args.i64("engine-cpu", 4)));
  if (!args.has("in")) tools::Args::die("--in <file> is required (or --jitter SECONDS)");
  const auto symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  const double rate = args.f64("rate", 0);
  const auto warmup = args.u64("warmup", 200'000);
  const int feed_cpu = static_cast<int>(args.i64("feed-cpu", 2));
  const int engine_cpu = static_cast<int>(args.i64("engine-cpu", 4));
  const int md_cpu = static_cast<int>(args.i64("md-cpu", 6));
  const auto qcap = args.u64("queue", 1 << 16);
  const auto prefetch = args.u64("prefetch", 8);

  const std::vector<std::byte> wire_buf = read_file(args.str("in", ""));
  const double ns_per_tick = calibrate_ns_per_tick();
  std::printf("capture: %zu bytes; TSC %.4f ns/tick; rate %s\n", wire_buf.size(), ns_per_tick,
              rate > 0 ? (std::to_string(static_cast<long long>(rate)) + " msg/s").c_str() : "unthrottled");

  SpscQueue<Command> q_in(qcap);
  SpscQueue<MdMsg> q_md(qcap);
  MatchingEngine<DefaultBook> engine(symbols, BookConfig{});

  std::atomic<bool> feed_done{false}, engine_done{false};
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::uint64_t fed = 0, malformed = 0, in_stalls = 0;
  std::uint64_t processed = 0;
  std::uint64_t t_start = 0, t_engine_end = 0;
  CrossCoreLatency h_match, h_md;
  EngineSink sink{q_md, 0, 0, {}};
  std::uint64_t md_msgs = 0;
  EventDigest md_digest;
  bool pinned[3] = {};

  std::thread feed([&] {
    set_thread_name("exsim-feed");
    pinned[0] = pin_current_thread(feed_cpu);
    ready.fetch_add(1);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    const double ticks_per_msg = rate > 0 ? 1e9 / rate / ns_per_tick : 0;
    const std::uint64_t t0 = rdtsc();
    t_start = t0;
    std::size_t off = 0;
    std::uint64_t i = 0;
    while (off < wire_buf.size()) {
      std::uint64_t stamp;
      if (rate > 0) {
        stamp = t0 + static_cast<std::uint64_t>(static_cast<double>(i) * ticks_per_msg);
        while (rdtsc() < stamp) cpu_relax();
      } else {
        stamp = rdtsc();
      }
      Command c;
      const auto r = wire::decode(wire_buf.data() + off, wire_buf.size() - off, c);
      if (r.status != wire::DecodeStatus::Ok) {
        if (r.consumed == 0) break;
        ++malformed;
        off += r.consumed;
        continue;
      }
      off += r.consumed;
      c.ts = stamp;
      while (!q_in.try_push(c)) {
        ++in_stalls;
        cpu_relax();
      }
      ++fed;
      ++i;
    }
    feed_done.store(true, std::memory_order_release);
  });

  std::thread matcher([&] {
    set_thread_name("exsim-engine");
    pinned[1] = pin_current_thread(engine_cpu);
    std::vector<Tob> tob(symbols);
    ready.fetch_add(1);
    for (;;) {
      Command* c = q_in.front();
      if (c == nullptr) {
        if (feed_done.load(std::memory_order_acquire) && q_in.front() == nullptr) break;
        cpu_relax();
        continue;
      }
      if (prefetch != 0) q_in.prefetch(prefetch);
      sink.ingress = c->ts;
      engine.process(*c, sink);
      if (c->symbol < symbols) {  // publish top-of-book if it changed
        const auto& b = engine.book(c->symbol);
        const Tob now{b.best_bid().value_or(INT64_MIN), b.best_ask().value_or(INT64_MIN), b.best_bid_qty(),
                      b.best_ask_qty()};
        if (!(now == tob[c->symbol])) {
          tob[c->symbol] = now;
          sink.publish(MdMsg{c->ts, now.bid, now.ask, now.bid_qty, now.ask_qty, c->symbol, MdKind::Bbo, Side::Buy, 0});
        }
      }
      const std::uint64_t t = rdtsc();
      if (processed >= warmup) h_match.record(t, c->ts);
      ++processed;
      q_in.pop();
    }
    t_engine_end = rdtsc();
    engine_done.store(true, std::memory_order_release);
  });

  std::thread publisher([&] {
    set_thread_name("exsim-md");
    pinned[2] = pin_current_thread(md_cpu);
    ready.fetch_add(1);
    for (;;) {
      MdMsg* m = q_md.front();
      if (m == nullptr) {
        if (engine_done.load(std::memory_order_acquire) && q_md.front() == nullptr) break;
        cpu_relax();
        continue;
      }
      const std::uint64_t t = rdtsc();
      if (md_msgs >= warmup) h_md.record(t, m->ingress_ts);
      Event as_event{};  // fold into a digest so the consumer does real work on every field
      as_event.price = m->px_a ^ m->px_b;
      as_event.qty = static_cast<Qty>(m->qty_a ^ m->qty_b);
      as_event.symbol = m->symbol;
      md_digest.on_event(as_event);
      ++md_msgs;
      q_md.pop();
    }
  });

  while (ready.load() != 3) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  feed.join();
  matcher.join();
  publisher.join();

  const double secs = static_cast<double>(t_engine_end - t_start) * ns_per_tick * 1e-9;
  std::printf("threads pinned: feed=%s(cpu %d) engine=%s(cpu %d) md=%s(cpu %d)\n", pinned[0] ? "yes" : "no", feed_cpu,
              pinned[1] ? "yes" : "no", engine_cpu, pinned[2] ? "yes" : "no", md_cpu);
  std::printf("messages: %llu (malformed %llu); md messages: %llu\n", static_cast<unsigned long long>(processed),
              static_cast<unsigned long long>(malformed), static_cast<unsigned long long>(md_msgs));
  std::printf("throughput: %.2f M msg/s end-to-end (%.3f s)\n", static_cast<double>(processed) / secs / 1e6, secs);
  std::printf("backpressure spins: ingress ring %llu, md ring %llu\n", static_cast<unsigned long long>(in_stalls),
              static_cast<unsigned long long>(sink.md_stalls));
  std::printf("latency (first %llu samples excluded as warmup):\n", static_cast<unsigned long long>(warmup));
  print_hist("ingress -> match complete", h_match.hist, ns_per_tick);
  print_hist("ingress -> md consumed", h_md.hist, ns_per_tick);
  if (h_match.negative + h_md.negative != 0)
    std::printf("  (%llu samples had negative cross-core TSC deltas: vCPU clock skew; recorded as 0)\n",
                static_cast<unsigned long long>(h_match.negative + h_md.negative));
  std::printf("engine event digest: %016llx (must equal exsim_replay)\n",
              static_cast<unsigned long long>(sink.counts.digest()));
  return 0;
}
