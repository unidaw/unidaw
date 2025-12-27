#include "apps/event_ring.h"

#include <atomic>

namespace daw {
namespace {

bool isPowerOfTwo(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

EventEntry* ringEntries(RingHeader* header) {
  auto* base = reinterpret_cast<uint8_t*>(header);
  const size_t offset = alignUp(sizeof(RingHeader), 64);
  return reinterpret_cast<EventEntry*>(base + offset);
}

}  // namespace

EventRingView makeEventRing(void* base, uint64_t offset) {
  EventRingView view;
  view.header = reinterpret_cast<RingHeader*>(
      reinterpret_cast<uint8_t*>(base) + offset);
  view.entries = ringEntries(view.header);
  view.mask = view.header && isPowerOfTwo(view.header->capacity)
                  ? (view.header->capacity - 1)
                  : 0;
  return view;
}

bool ringWrite(EventRingView& ring, const EventEntry& entry) {
  if (!ring.header || ring.mask == 0) {
    return false;
  }
  const uint32_t write = ring.header->writeIndex.load(std::memory_order_relaxed);
  const uint32_t read = ring.header->readIndex.load(std::memory_order_acquire);
  const uint32_t next = (write + 1) & ring.mask;
  if (next == read) {
    return false;
  }
  ring.entries[write] = entry;
  ring.header->writeIndex.store(next, std::memory_order_release);
  return true;
}

bool ringPeek(const EventRingView& ring, EventEntry& entry) {
  if (!ring.header || ring.mask == 0) {
    return false;
  }
  const uint32_t read = ring.header->readIndex.load(std::memory_order_relaxed);
  const uint32_t write = ring.header->writeIndex.load(std::memory_order_acquire);
  if (read == write) {
    return false;
  }
  entry = ring.entries[read];
  return true;
}

bool ringPop(EventRingView& ring, EventEntry& entry) {
  if (!ringPeek(ring, entry)) {
    return false;
  }
  const uint32_t read = ring.header->readIndex.load(std::memory_order_relaxed);
  ring.header->readIndex.store((read + 1) & ring.mask, std::memory_order_release);
  return true;
}

}  // namespace daw
