// Minimal `--key value` / `--flag` argument parsing for the command-line tools.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace exsim::tools {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      if (k.rfind("--", 0) != 0) die("unexpected argument: " + k);
      k = k.substr(2);
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
        kv_[k] = argv[++i];
      else
        kv_[k] = "";
    }
  }
  bool has(const std::string& k) const { return kv_.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  std::uint64_t u64(const std::string& k, std::uint64_t def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    char* end = nullptr;
    const auto v = std::strtoull(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || *end != '\0') die("--" + k + " expects an integer");
    return v;
  }
  std::int64_t i64(const std::string& k, std::int64_t def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::strtoll(it->second.c_str(), nullptr, 10);
  }
  double f64(const std::string& k, double def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::strtod(it->second.c_str(), nullptr);
  }

  [[noreturn]] static void die(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n", msg.c_str());
    std::exit(2);
  }

 private:
  std::map<std::string, std::string> kv_;
};

}  // namespace exsim::tools
