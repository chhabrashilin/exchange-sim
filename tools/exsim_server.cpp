// exsim_server: a TCP order-entry gateway around the matching engine (Linux, epoll).
//
//   client --(binary orders)--> [decode] -> [stamp owner/ts/seq] -> [JOURNAL] -> [risk] -> [engine]
//   client <--(exec reports)--------------------------------------------------- [events]
//
// Ordering guarantees (the point of the design):
//   * Write-ahead: a command is appended to the journal BEFORE the engine sees it, and the journal is
//     flushed (per the --sync policy) BEFORE any response for it leaves the process. So a client never
//     holds an acknowledgement for a command that a crash could erase.
//   * Determinism: the engine and risk gate are pure functions of the journaled command stream, so
//     `--recover` rebuilds the exact pre-crash state by replaying the journal.
//
// The connection id is the participant (owner) id; clients cannot choose it.
//
//   exsim_server --journal j.bin [--port 9000] [--symbols 8] [--recover] [--sync os|batch|every]
//                [--batch 1000] [--cpu N] [--risk]
//
// --sync os      flush to the OS after each network batch (survives kill -9, not power loss)   [default]
// --sync batch   also fsync every --batch commands
// --sync every   fsync every command (survives power loss; slow)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "args.hpp"
#include "exsim/cpu.hpp"
#include "exsim/journal.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/risk.hpp"
#include "exsim/sinks.hpp"

using namespace exsim;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Conn {
  int fd = -1;
  OwnerId owner = 0;
  std::vector<std::byte> in;
  std::size_t in_len = 0;
  std::vector<std::byte> out;
  std::size_t out_off = 0;
  bool want_out = false, paused = false;
};

struct ReportSink {
  std::vector<std::byte>& out;
  std::uint64_t client_seq;
  EventDigest& digest;
  void on_event(const Event& e) {
    const std::size_t at = out.size();
    out.resize(at + wire::kReportSize);
    wire::encode_report(e, client_seq, out.data() + at);
    digest.on_event(e);
  }
};

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

}  // namespace

int main(int argc, char** argv) {
  const tools::Args args(argc, argv);
  if (!args.has("journal")) tools::Args::die("--journal <file> is required");
  const std::string jpath = args.str("journal", "");
  const auto port = static_cast<std::uint16_t>(args.u64("port", 9000));
  const auto symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  const std::string sync_mode = args.str("sync", "os");
  const std::uint64_t batch = args.u64("batch", 1000);
  if (sync_mode != "os" && sync_mode != "batch" && sync_mode != "every") tools::Args::die("--sync must be os, batch or every");
  if (args.has("cpu")) pin_current_thread(static_cast<int>(args.i64("cpu", 0)));
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  MatchingEngine<DefaultBook> engine(symbols, BookConfig{});
  RiskConfig rc;
  if (args.has("risk")) {
    rc.max_qty = 100'000, rc.collar_ticks = 5'000, rc.rate_per_sec = 5'000'000, rc.burst = 1'000;
  }
  RiskGate<MatchingEngine<DefaultBook>> gate(engine, rc);
  EventDigest digest;
  std::uint64_t seq = 0;

  // ---- recovery: replay the journal into the fresh engine ----
  std::unique_ptr<JournalWriter> journal;
  if (args.has("recover") && std::filesystem::exists(jpath)) {
    struct Tap { EventDigest& d; void on_event(const Event& e) { d.on_event(e); } } tap{digest};
    const auto t0 = std::chrono::steady_clock::now();
    JournalScan scan;
    // replay while scanning (single pass), then reopen for append with any torn tail trimmed
    scan = journal_scan(jpath, [&](const Command& c) {
      gate.process(c, tap);
      seq = c.seq;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    journal = std::make_unique<JournalWriter>(JournalWriter::recover(jpath));
    std::printf("RECOVERED records=%llu status=%s digest=%016llx last_seq=%llu replay_s=%.3f\n",
                static_cast<unsigned long long>(scan.records),
                scan.status == JournalStatus::Clean ? "clean" : scan.status == JournalStatus::TornTail ? "torn-tail-discarded" : "CORRUPT-tail-discarded",
                static_cast<unsigned long long>(digest.value()), static_cast<unsigned long long>(seq), secs);
  } else {
    journal = std::make_unique<JournalWriter>(JournalWriter::create(jpath));
  }

  // ---- sockets ----
  const int lfd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET, addr.sin_port = htons(port), addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(lfd, 64) != 0) {
    std::perror("bind/listen");
    return 1;
  }
  set_nonblocking(lfd);
  const int ep = epoll_create1(0);
  epoll_event ev{};
  ev.events = EPOLLIN, ev.data.fd = lfd;
  epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
  std::printf("READY port=%u symbols=%u sync=%s journal=%s\n", port, symbols, sync_mode.c_str(), jpath.c_str());
  std::fflush(stdout);

  std::unordered_map<int, Conn> conns;
  OwnerId next_owner = 1;
  std::uint64_t handled = 0, since_sync = 0, malformed = 0;
  std::vector<std::byte> chunk(1 << 16);

  auto close_conn = [&](int fd) {
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns.erase(fd);
  };
  auto rearm = [&](Conn& c) {
    epoll_event e{};
    e.events = (c.paused ? 0u : static_cast<unsigned>(EPOLLIN)) | (c.want_out ? static_cast<unsigned>(EPOLLOUT) : 0u);
    e.data.fd = c.fd;
    epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &e);
  };
  auto try_write = [&](Conn& c) {
    while (c.out_off < c.out.size()) {
      const ssize_t n = send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
      if (n > 0) { c.out_off += static_cast<std::size_t>(n); continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      return false;  // connection error
    }
    if (c.out_off == c.out.size()) c.out.clear(), c.out_off = 0;
    const bool backlog = c.out.size() - c.out_off > (16u << 20);
    if (c.want_out != (c.out_off < c.out.size()) || c.paused != backlog) {
      c.want_out = c.out_off < c.out.size(), c.paused = backlog;
      rearm(c);
    }
    return true;
  };

  epoll_event events[64];
  while (!g_stop) {
    const int n = epoll_wait(ep, events, 64, 200);
    bool journaled_any = false;
    std::vector<int> touched;
    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      if (fd == lfd) {
        for (;;) {
          const int cfd = accept(lfd, nullptr, nullptr);
          if (cfd < 0) break;
          set_nonblocking(cfd);
          setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          Conn c;
          c.fd = cfd, c.owner = next_owner++;
          conns.emplace(cfd, std::move(c));
          epoll_event ce{};
          ce.events = EPOLLIN, ce.data.fd = cfd;
          epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ce);
        }
        continue;
      }
      auto it = conns.find(fd);
      if (it == conns.end()) continue;
      Conn& c = it->second;
      if (events[i].events & EPOLLOUT) {
        if (!try_write(c)) { close_conn(fd); continue; }
      }
      if (!(events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))) continue;
      const ssize_t got = recv(fd, chunk.data(), chunk.size(), 0);
      if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close_conn(fd); continue; }
      if (got < 0) continue;
      c.in.resize(c.in_len + static_cast<std::size_t>(got));
      std::memcpy(c.in.data() + c.in_len, chunk.data(), static_cast<std::size_t>(got));
      c.in_len += static_cast<std::size_t>(got);

      std::size_t off = 0;
      bool broken = false;
      while (off < c.in_len) {
        Command cmd;
        const auto r = wire::decode(c.in.data() + off, c.in_len - off, cmd);
        if (r.status == wire::DecodeStatus::Incomplete) break;
        if (r.status == wire::DecodeStatus::Malformed) {
          ++malformed;
          if (r.consumed == 0) { broken = true; break; }  // framing lost: drop the connection
          off += r.consumed;
          continue;
        }
        off += r.consumed;
        const std::uint64_t client_seq = cmd.seq;
        cmd.seq = ++seq;              // the sequencer's order is THE order
        cmd.owner = c.owner;          // identity comes from the session, never from the client
        cmd.ts = now_ns();
        journal->append(cmd);         // write-ahead
        journaled_any = true;
        ReportSink sink{c.out, client_seq, digest};
        gate.process(cmd, sink);
        const std::size_t at = c.out.size();
        c.out.resize(at + wire::kReportSize);
        wire::encode_done(cmd.symbol, client_seq, c.out.data() + at);
        ++handled;
        if (sync_mode == "every") journal->sync();
        else if (sync_mode == "batch" && ++since_sync >= batch) journal->sync(), since_sync = 0;
      }
      if (broken) { close_conn(fd); continue; }
      c.in_len -= off;
      std::memmove(c.in.data(), c.in.data() + off, c.in_len);
      touched.push_back(fd);
    }
    // Flush the journal to the OS BEFORE any response leaves the process.
    if (journaled_any) journal->flush();
    for (int fd : touched) {
      auto it = conns.find(fd);
      if (it != conns.end() && !try_write(it->second)) close_conn(fd);
    }
  }

  journal->sync();
  std::printf("STOPPED handled=%llu malformed=%llu risk_rejects=%llu digest=%016llx seq=%llu\n",
              static_cast<unsigned long long>(handled), static_cast<unsigned long long>(malformed),
              static_cast<unsigned long long>(gate.rejected()), static_cast<unsigned long long>(digest.value()),
              static_cast<unsigned long long>(seq));
  return 0;
}
