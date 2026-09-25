// exsim_replay: deterministic replay of a binary capture through a chosen book implementation.
//
// Prints the event-stream digest. The same capture must produce the same digest on every run and
// under every implementation (ref, aos, soa, hybrid). That is the determinism and equivalence check.
// With --journal, every output event is also written as a raw 40-byte record.
//
//   exsim_replay --in flow.bin [--book hybrid|aos|soa|ref] [--symbols 8] [--journal events.bin]

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "args.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/sinks.hpp"
#include "exsim/wire_file.hpp"

namespace {

struct JournalSink {
  exsim::CountingSink counts;
  std::ofstream* journal;
  void on_event(const exsim::Event& e) {
    counts.on_event(e);
    if (journal != nullptr) journal->write(reinterpret_cast<const char*>(&e), sizeof e);
  }
};

template <class Book>
int replay(const std::vector<exsim::Command>& cmds, std::uint32_t symbols, std::ofstream* journal) {
  using namespace exsim;
  MatchingEngine<Book> engine(symbols, BookConfig{});
  JournalSink sink{{}, journal};
  const auto t0 = std::chrono::steady_clock::now();
  for (const Command& c : cmds) engine.process(c, sink);
  const auto t1 = std::chrono::steady_clock::now();
  const double s = std::chrono::duration<double>(t1 - t0).count();
  const auto& k = sink.counts;
  std::printf("events: %llu  (accepted %llu, rejected %llu, trades %llu, canceled %llu, modified %llu)\n",
              static_cast<unsigned long long>(k.events()), static_cast<unsigned long long>(k.count(EventType::Accepted)),
              static_cast<unsigned long long>(k.count(EventType::Rejected)),
              static_cast<unsigned long long>(k.count(EventType::Trade)),
              static_cast<unsigned long long>(k.count(EventType::Canceled)),
              static_cast<unsigned long long>(k.count(EventType::Modified)));
  std::printf("traded volume: %llu lots\n", static_cast<unsigned long long>(k.volume()));
  std::printf("replay time: %.3f s (%.2f M msg/s, single pass, untuned)\n", s, static_cast<double>(cmds.size()) / s / 1e6);
  std::printf("digest: %016llx\n", static_cast<unsigned long long>(k.digest()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace exsim;
  const tools::Args args(argc, argv);
  if (!args.has("in")) tools::Args::die("--in <file> is required");
  const std::string book = args.str("book", "hybrid");
  const auto symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));

  DecodeStats st;
  const auto cmds = decode_stream(read_file(args.str("in", "")), &st);
  std::printf("decoded %llu messages (%llu malformed%s)\n", static_cast<unsigned long long>(st.ok),
              static_cast<unsigned long long>(st.malformed), st.truncated ? ", TRUNCATED" : "");

  std::ofstream journal;
  if (args.has("journal")) {
    journal.open(args.str("journal", ""), std::ios::binary);
    if (!journal) tools::Args::die("cannot open journal file");
  }
  std::ofstream* jp = journal.is_open() ? &journal : nullptr;

  std::printf("book: %s\n", book.c_str());
  if (book == "hybrid") return replay<HybridBook>(cmds, symbols, jp);
  if (book == "aos") return replay<AosBook>(cmds, symbols, jp);
  if (book == "soa") return replay<SoaBook>(cmds, symbols, jp);
  if (book == "ref") return replay<ReferenceBook>(cmds, symbols, jp);
  tools::Args::die("--book must be one of hybrid, aos, soa, ref");
}
