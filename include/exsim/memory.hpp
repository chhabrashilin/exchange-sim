// Fixed-size, page-backed storage for the engine's large arrays.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace exsim {

// A fixed-size, zero-initialized array that never reallocates.
//
// Every page is touched once at construction (the memset below), so page faults happen at startup
// and never on the matching path. On Linux, arrays of 2 MiB or more are mmap'd and advised for
// transparent huge pages, which cuts dTLB misses on the order store and the id index.
template <class T>
class PageArray {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

 public:
  PageArray() = default;
  explicit PageArray(std::size_t n) : n_(n), bytes_(round_up(n * sizeof(T))) {
    if (n == 0) return;
#if defined(__linux__)
    void* p = map_aligned(bytes_);
#else
    void* p = ::operator new(bytes_, std::align_val_t{kPage});
#endif
    std::memset(p, 0, bytes_);
    data_ = static_cast<T*>(p);
  }
  ~PageArray() { release(); }

  PageArray(const PageArray&) = delete;
  PageArray& operator=(const PageArray&) = delete;
  PageArray(PageArray&& o) noexcept
      : data_(std::exchange(o.data_, nullptr)), n_(std::exchange(o.n_, 0)), bytes_(std::exchange(o.bytes_, 0)) {}
  PageArray& operator=(PageArray&& o) noexcept {
    if (this != &o) {
      release();
      data_ = std::exchange(o.data_, nullptr);
      n_ = std::exchange(o.n_, 0);
      bytes_ = std::exchange(o.bytes_, 0);
    }
    return *this;
  }

  T& operator[](std::size_t i) noexcept { return data_[i]; }
  const T& operator[](std::size_t i) const noexcept { return data_[i]; }
  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return n_; }
  std::size_t bytes() const noexcept { return bytes_; }

 private:
  static constexpr std::size_t kPage = 4096;
  static constexpr std::size_t kHugePage = 2u << 20;

  static std::size_t round_up(std::size_t b) noexcept {
    const std::size_t a = b >= kHugePage ? kHugePage : kPage;
    return (b + a - 1) & ~(a - 1);
  }

#if defined(__linux__)
  // Transparent huge pages can only back 2 MiB-aligned ranges, and mmap guarantees only 4 KiB
  // alignment. So over-map by one huge page, trim the misaligned head and tail, then advise.
  static void* map_aligned(std::size_t bytes) {
    const std::size_t align = bytes >= kHugePage ? kHugePage : kPage;
    const std::size_t span = bytes + align;
    void* raw = ::mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::bad_alloc();
    const auto base = reinterpret_cast<std::uintptr_t>(raw);
    const std::uintptr_t start = (base + align - 1) & ~(align - 1);
    if (start > base) ::munmap(raw, start - base);
    const std::size_t tail = (base + span) - (start + bytes);
    if (tail > 0) ::munmap(reinterpret_cast<void*>(start + bytes), tail);
    if (align == kHugePage) ::madvise(reinterpret_cast<void*>(start), bytes, MADV_HUGEPAGE);
    return reinterpret_cast<void*>(start);
  }
#endif

  void release() noexcept {
    if (data_ == nullptr) return;
#if defined(__linux__)
    ::munmap(data_, bytes_);
#else
    ::operator delete(data_, std::align_val_t{kPage});
#endif
    data_ = nullptr;
  }

  T* data_ = nullptr;
  std::size_t n_ = 0;
  std::size_t bytes_ = 0;
};

}  // namespace exsim
