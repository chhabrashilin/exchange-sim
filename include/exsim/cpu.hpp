// Thread placement helpers.
#pragma once

#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace exsim {

// Pins the calling thread to one logical CPU, so it keeps its caches warm and is never migrated
// mid-burst. Returns false where unsupported or when the CPU is not in the process's allowed set.
inline bool pin_current_thread(int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

inline void set_thread_name(const char* name) noexcept {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), name);  // truncated to 15 chars by the kernel
#else
  (void)name;
#endif
}

inline unsigned hardware_threads() noexcept {
  const unsigned n = std::thread::hardware_concurrency();
  return n ? n : 1;
}

}  // namespace exsim
