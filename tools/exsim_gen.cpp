// exsim_gen: writes a deterministic synthetic order-flow capture in the binary wire format.
//
//   exsim_gen --out flow.bin [--messages 10000000] [--symbols 8] [--seed 42] [--target-live 4096]

#include <cstdio>

#include "args.hpp"
#include "exsim/wire_file.hpp"
#include "exsim/workload.hpp"

int main(int argc, char** argv) {
  using namespace exsim;
  const tools::Args args(argc, argv);
  if (!args.has("out")) tools::Args::die("--out <file> is required");

  WorkloadConfig cfg;
  cfg.messages = args.u64("messages", 10'000'000);
  cfg.symbols = static_cast<std::uint32_t>(args.u64("symbols", 8));
  cfg.seed = args.u64("seed", 42);
  cfg.target_live = static_cast<std::uint32_t>(args.u64("target-live", 4096));

  WorkloadMix mix;
  const auto cmds = generate_workload(cfg, &mix);
  const auto buf = encode_stream(cmds);
  write_file(args.str("out", ""), buf);

  const double n = static_cast<double>(cmds.size());
  std::printf("wrote %zu messages (%zu bytes) to %s\n", cmds.size(), buf.size(), args.str("out", "").c_str());
  std::printf("mix: new %.1f%% (marketable %.1f%%, ioc/fok/market %.1f%%), cancel %.1f%%, modify %.1f%%\n",
              100.0 * static_cast<double>(mix.new_orders) / n, 100.0 * static_cast<double>(mix.marketable) / n,
              100.0 * static_cast<double>(mix.ioc_fok_market) / n, 100.0 * static_cast<double>(mix.cancels) / n,
              100.0 * static_cast<double>(mix.modifies) / n);
  return 0;
}
