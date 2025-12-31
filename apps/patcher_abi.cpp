#include "apps/patcher_abi.h"

namespace daw {

extern "C" void atomic_store_u64(uint64_t* ptr, uint64_t value) {
  if (!ptr) {
    return;
  }
  auto* atomicPtr = reinterpret_cast<std::atomic<uint64_t>*>(ptr);
  atomicPtr->store(value, std::memory_order_relaxed);
}

}  // namespace daw
