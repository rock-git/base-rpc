#pragma once
// base-rpc shim: plain libc allocation. Replaces 3FS's GlobalMemoryAllocator (jemalloc/mimalloc dlopen + metrics).
// shortcut: no custom allocator, swap in your own if you need one.
#include <cerrno>
#include <cstddef>
#include <cstdlib>

namespace hf3fs::memory {

inline void *allocate(size_t size) { return std::malloc(size); }

inline void deallocate(void *mem) { return std::free(mem); }

inline void *memalign(size_t alignment, size_t size) {
  void *ptr = nullptr;
  int rc = posix_memalign(&ptr, alignment, size);
  return rc == 0 ? (errno = 0, ptr) : (errno = rc, nullptr);
}

}  // namespace hf3fs::memory
