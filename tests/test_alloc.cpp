// Enforces the zero-allocation hot path: after construction, processing any message stream must not
// touch the heap. test_main.cpp's operator new counts every allocation in the process.

#include <atomic>
#include <cstdio>

#include "exsim/workload.hpp"
#include "helpers.hpp"

extern std::atomic<unsigned long long> g_test_allocs;

using namespace exsim;

namespace {

template <class Book>
unsigned long long allocs_while_matching(const std::vector<Command>& cmds, const WorkloadConfig& wc) {
  MatchingEngine<Book> engine(wc.symbols, wc.book);
  CountingSink sink;
  const auto before = g_test_allocs.load();
  for (const Command& c : cmds) engine.process(c, sink);
  return g_test_allocs.load() - before;
}

}  // namespace

TEST(hot_path_never_allocates) {
  if (EXSIM_TSAN_BUILD) {
    std::printf("      skipped: TSan supplies its own operator new, so allocations are not counted here\n");
    return;
  }
  WorkloadConfig wc;
  wc.messages = 300'000;
  wc.symbols = 4;
  const auto cmds = generate_workload(wc);
  CHECK_EQ(allocs_while_matching<AosScatterBook>(cmds, wc), 0ull);
  CHECK_EQ(allocs_while_matching<AosBook>(cmds, wc), 0ull);
  CHECK_EQ(allocs_while_matching<SoaBook>(cmds, wc), 0ull);
  CHECK_EQ(allocs_while_matching<HybridBook>(cmds, wc), 0ull);
  // Sanity check that the counter works: the node-based reference book allocates constantly.
  CHECK(allocs_while_matching<ReferenceBook>(cmds, wc) > 10'000ull);
}
