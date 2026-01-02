#include "apps/patcher_abi.h"

namespace daw {

extern "C" void atomic_store_u64(uint64_t* ptr, uint64_t value) {
  if (!ptr) {
    return;
  }
#if defined(__clang__) || defined(__GNUC__)
  __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
#else
  auto* atomicPtr = reinterpret_cast<std::atomic<uint64_t>*>(ptr);
  atomicPtr->store(value, std::memory_order_relaxed);
#endif
}

}  // namespace daw
