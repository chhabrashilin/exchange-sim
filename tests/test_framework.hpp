// A deliberately tiny test harness: self-registering TEST cases and CHECK macros that report
// file:line and operand values. It needs no dependencies and no network, so the suite builds anywhere
// the engine does.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace exsim::test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

struct Failure {};

inline int& failures() {
  static int f = 0;
  return f;
}
inline long long& assertions() {
  static long long a = 0;
  return a;
}

inline void report(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <class T>
std::string show(const T& v) {
  if constexpr (requires(std::ostream& os) { os << v; }) {
    std::ostringstream os;
    os << v;
    return os.str();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else {
    return "<?>";
  }
}

template <class A, class B>
__attribute__((noinline)) void check_eq(A a, B b, const char* ea, const char* eb, const char* file, int line) {
  ++assertions();
  if (!(a == b)) report(file, line, std::string("CHECK_EQ(") + ea + ", " + eb + "): " + show(a) + " != " + show(b));
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace exsim::test

#define EXSIM_CAT2(a, b) a##b
#define EXSIM_CAT(a, b) EXSIM_CAT2(a, b)

#define TEST(name)                                                                        \
  static void name();                                                                     \
  static const ::exsim::test::Registrar EXSIM_CAT(name, _registrar){#name, &name};        \
  static void name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++::exsim::test::assertions();                                               \
    if (!(cond)) ::exsim::test::report(__FILE__, __LINE__, "CHECK(" #cond ")");  \
  } while (0)

// Operands are evaluated once and passed by value (an operand may refer into a temporary). The
// comparison and formatting live in an out-of-line template, so each CHECK_EQ expands to one call.
#define CHECK_EQ(a, b) ::exsim::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

// Like CHECK, but aborts the current test case on failure.
#define REQUIRE(cond)                                                              \
  do {                                                                             \
    ++::exsim::test::assertions();                                                 \
    if (!(cond)) {                                                                 \
      ::exsim::test::report(__FILE__, __LINE__, "REQUIRE(" #cond ")");             \
      throw ::exsim::test::Failure{};                                              \
    }                                                                              \
  } while (0)
