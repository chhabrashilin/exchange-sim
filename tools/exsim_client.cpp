// exsim_client: load generator and verifier for exsim_server.
//
// Sends a deterministic order-flow stream over TCP and measures round-trip time: from the moment a
// command is (or was scheduled to be) sent until its Done report arrives. Two modes:
//
//   --rate R     open loop: command i is scheduled at t0 + i/R and its latency is measured from the
//                SCHEDULED time, so a slow server shows up as latency instead of silently slowing the
//                sender down ("coordinated omission").
//   --window W   closed loop: keep W commands in flight; measures throughput.
//
// Correctness check: the client runs the same commands through its own local engine and compares an
// order-sensitive digest of the reports it received against the digest of the events the local engine
// produced. They must match, so the server's output is verified byte-for-byte, not just counted.
// (Owner ids differ between client and server, which is irrelevant with self-trade prevention off.)
//
//   exsim_client --port 9000 --messages 1000000 (--rate 200000 | --window 256) [--symbols 8] [--seed 42]

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "args.hpp"
#include "exsim/clock.hpp"
#include "exsim/cpu.hpp"
#include "exsim/latency_histogram.hpp"
#include "exsim/protocol.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"
#include "exsim/workload.hpp"

using namespace exsim;

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  const auto port = static_cast<std::uint16_t>(args.u64("port", 9000));
  const auto symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  const double rate = args.f64("rate", 0);
  const std::uint64_t window = args.u64("window", 256);
  const bool verify = !args.has("no-verify");
  if (args.has("cpu")) pin_current_thread(static_cast<int>(args.i64("cpu", 0)));

  WorkloadConfig wc;
  wc.messages = args.u64("messages", 1'000'000);
  wc.symbols = symbols;
  wc.seed = args.u64("seed", 42);
  auto cmds = generate_workload(wc);
  for (std::size_t i = 0; i < cmds.size(); ++i) cmds[i].seq = i + 1;  // client sequence numbers

  std::uint64_t expected_digest = 0, expected_events = 0;
  if (verify) {
    MatchingEngine<DefaultBook> local(symbols, BookConfig{});
    CountingSink s;
    for (const Command& c : cmds) local.process(c, s);
    expected_digest = s.digest(), expected_events = s.events();
  }
  std::vector<std::byte> wire_buf;
  std::vector<std::size_t> offset(cmds.size() + 1);
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    offset[i] = wire_buf.size();
    std::byte tmp[wire::kMaxMessageSize];
    const std::size_t n = wire::encode(cmds[i], tmp);
    wire_buf.insert(wire_buf.end(), tmp, tmp + n);
  }
  offset[cmds.size()] = wire_buf.size();

  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET, addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { std::perror("connect"); return 1; }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

  const double ns_per_tick = calibrate_ns_per_tick();
  const std::uint64_t ticks_per_msg = rate > 0 ? static_cast<std::uint64_t>(1e9 / rate / ns_per_tick) : 0;
  std::vector<std::uint64_t> stamp(cmds.size());
  LatencyHistogram hist;
  EventDigest got_digest;
  std::uint64_t sent = 0, done = 0, reports = 0, rejects = 0, risk_rejects = 0;
  std::size_t send_off = 0;
  std::vector<std::byte> in(1 << 20);
  std::size_t in_len = 0;
  const std::uint64_t warmup = wc.messages / 20;
  const std::uint64_t t0 = rdtsc();
  bool closed = false;

  while (done < cmds.size() && !closed) {
    // ---- send ----
    while (sent < cmds.size()) {
      if (send_off == 0) {  // starting a new command: decide whether it is due, and stamp it
        if (rate > 0) {
          const std::uint64_t due = t0 + sent * ticks_per_msg;
          if (rdtsc() < due) break;
          stamp[sent] = due;  // latency is measured from the schedule, not from when we got around to it
        } else {
          if (sent - done >= window) break;
          stamp[sent] = rdtsc();
        }
      }
      const std::size_t len = offset[sent + 1] - offset[sent];
      const ssize_t n = send(fd, wire_buf.data() + offset[sent] + send_off, len - send_off, MSG_NOSIGNAL);
      if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true; break; }
      send_off += static_cast<std::size_t>(n);
      if (send_off < len) break;  // socket buffer full mid-message: resume next iteration
      send_off = 0;
      ++sent;
    }
    // ---- receive ----
    pollfd p{fd, POLLIN, 0};
    poll(&p, 1, sent < cmds.size() && rate > 0 ? 0 : 1);
    const ssize_t got = recv(fd, in.data() + in_len, in.size() - in_len, 0);
    if (got == 0) { closed = true; }
    else if (got > 0) in_len += static_cast<std::size_t>(got);
    std::size_t off = 0;
    while (in_len - off >= wire::kReportSize) {
      wire::ExecReport r;
      std::memcpy(&r, in.data() + off, sizeof r);
      off += sizeof r;
      Event e;
      if (wire::report_to_event(r, e)) {
        ++reports;
        got_digest.on_event(e);
        if (e.type == EventType::Rejected) ++rejects, risk_rejects += e.reason >= Reason::RiskMaxQty;
      } else {
        const std::uint64_t idx = r.hdr.seq - 1;
        if (idx >= warmup && idx < cmds.size()) hist.record(rdtsc() - stamp[idx]);
        ++done;
      }
    }
    std::memmove(in.data(), in.data() + off, in_len - off);
    in_len -= off;
  }
  const double secs = static_cast<double>(rdtsc() - t0) * ns_per_tick * 1e-9;
  close(fd);

  auto ns = [&](std::uint64_t t) { return static_cast<double>(t) * ns_per_tick; };
  std::printf("mode: %s\n", rate > 0 ? "open loop (latency from scheduled send time)" : "closed loop");
  std::printf("commands sent %llu, acknowledged (Done) %llu, exec reports %llu, rejects %llu (risk %llu)%s\n",
              static_cast<unsigned long long>(sent), static_cast<unsigned long long>(done),
              static_cast<unsigned long long>(reports), static_cast<unsigned long long>(rejects),
              static_cast<unsigned long long>(risk_rejects), closed && done < cmds.size() ? "  [connection closed early]" : "");
  std::printf("throughput: %.3f M cmds/s over %.2f s\n", static_cast<double>(done) / secs / 1e6, secs);
  std::printf("round-trip latency (loopback TCP, first 5%% excluded): p50 %.1f us  p90 %.1f us  p99 %.1f us  p99.9 %.1f us  max %.1f us\n",
              ns(hist.percentile(50)) / 1e3, ns(hist.percentile(90)) / 1e3, ns(hist.percentile(99)) / 1e3,
              ns(hist.percentile(99.9)) / 1e3, ns(hist.max()) / 1e3);
  std::printf("ACKED %llu\n", static_cast<unsigned long long>(done));
  if (verify && done == cmds.size()) {
    const bool ok = got_digest.value() == expected_digest && got_digest.count() == expected_events;
    std::printf("verification: server output digest %016llx (%llu events) vs local engine %016llx (%llu events): %s\n",
                static_cast<unsigned long long>(got_digest.value()), static_cast<unsigned long long>(got_digest.count()),
                static_cast<unsigned long long>(expected_digest), static_cast<unsigned long long>(expected_events),
                ok ? "IDENTICAL" : "MISMATCH");
    return ok ? 0 : 2;
  }
  return closed && done < cmds.size() ? 4 : 0;
}
