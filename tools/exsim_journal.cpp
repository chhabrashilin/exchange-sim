// exsim_journal: inspect and replay a journal file offline.
//
//   exsim_journal verify  j.bin      CRC-check every record; report clean / torn tail / corrupt
//   exsim_journal replay  j.bin      replay into a fresh engine; print event digest and book state
//
// `replay` uses the same engine and risk configuration as exsim_server (defaults), so its digest can be
// compared with the digest the server prints after `--recover`.

#include <cstdio>
#include <string>

#include "exsim/journal.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/risk.hpp"
#include "exsim/sinks.hpp"

using namespace exsim;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: exsim_journal verify|replay <journal> [--symbols N] [--risk]\n");
    return 2;
  }
  const std::string cmd = argv[1], path = argv[2];
  std::uint32_t symbols = 8;
  bool risk = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "--symbols" && i + 1 < argc) symbols = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    if (std::string(argv[i]) == "--risk") risk = true;
  }
  try {
    if (cmd == "verify") {
      const auto s = journal_scan(path, [](const Command&) {});
      std::printf("records=%llu valid_bytes=%llu status=%s\n", static_cast<unsigned long long>(s.records),
                  static_cast<unsigned long long>(s.valid_bytes),
                  s.status == JournalStatus::Clean ? "clean" : s.status == JournalStatus::TornTail ? "torn-tail" : "CORRUPT");
      return s.status == JournalStatus::Corrupt ? 1 : 0;
    }
    if (cmd == "replay") {
      MatchingEngine<DefaultBook> engine(symbols, BookConfig{});
      RiskConfig rc;
      if (risk) rc.max_qty = 100'000, rc.collar_ticks = 5'000, rc.rate_per_sec = 5'000'000, rc.burst = 1'000;
      RiskGate<MatchingEngine<DefaultBook>> gate(engine, rc);
      CountingSink sink;
      std::uint64_t last_seq = 0;
      const auto s = journal_scan(path, [&](const Command& c) { gate.process(c, sink); last_seq = c.seq; });
      std::uint64_t resting = 0;
      for (std::uint32_t i = 0; i < symbols; ++i) resting += engine.book(i).order_count();
      std::printf("records=%llu status=%s last_seq=%llu events=%llu trades=%llu resting_orders=%llu digest=%016llx\n",
                  static_cast<unsigned long long>(s.records),
                  s.status == JournalStatus::Clean ? "clean" : s.status == JournalStatus::TornTail ? "torn-tail" : "CORRUPT",
                  static_cast<unsigned long long>(last_seq), static_cast<unsigned long long>(sink.events()),
                  static_cast<unsigned long long>(sink.count(EventType::Trade)), static_cast<unsigned long long>(resting),
                  static_cast<unsigned long long>(sink.digest()));
      return 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
