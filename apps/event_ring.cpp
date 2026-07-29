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

UiEditBatchEntry* editRingEntries(RingHeader* header) {
  auto* base = reinterpret_cast<uint8_t*>(header);
  const size_t offset = alignUp(sizeof(RingHeader), 64);
  return reinterpret_cast<UiEditBatchEntry*>(base + offset);
}

// EventEntry::ready is a plain uint32_t (EventEntry has to stay copyable), so the
// ring reaches it through the atomic builtins. Release on publish / acquire on read
// is what orders the entry's payload against the flag: a consumer that sees ready==1
// is guaranteed to see every byte the producer wrote before setting it.
inline void storeReady(uint32_t& slot, uint32_t value) {
  __atomic_store_n(&slot, value, __ATOMIC_RELEASE);
}
inline uint32_t loadReady(const uint32_t& slot) {
  return __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
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

UiEditRingView makeUiEditRing(void* base, uint64_t offset) {
  UiEditRingView view;
  view.header = reinterpret_cast<RingHeader*>(
      reinterpret_cast<uint8_t*>(base) + offset);
  view.entries = editRingEntries(view.header);
  // A stride mismatch means the peer mapped this region with a different
  // UiEditBatchEntry layout; mask 0 disables the ring rather than reading
  // entries at the wrong addresses.
  const bool ok = view.header != nullptr &&
                  isPowerOfTwo(view.header->capacity) &&
                  view.header->entrySize == sizeof(UiEditBatchEntry);
  view.mask = ok ? (view.header->capacity - 1) : 0;
  return view;
}

// M2.18: MULTI-PRODUCER. Reserve a slot by CAS on writeIndex, then fill it, then
// publish it with ready=1. The previous version read writeIndex, wrote the slot, and
// stored writeIndex back — so two producers racing both claimed the same index, both
// wrote the same slot, and exactly one command disappeared. That is the whole reason
// `daw-cli do` demanded --force.
//
// Reserving before filling means the consumer can reach a slot whose producer has not
// finished (or has died mid-write). It must not read that slot, so it waits on ready;
// see ringPeek and ringStalledSlot.
bool ringWrite(EventRingView& ring, const EventEntry& entry) {
  if (!ring.header || ring.mask == 0) {
    return false;
  }
  uint32_t write = ring.header->writeIndex.load(std::memory_order_relaxed);
  uint32_t next = 0;
  for (;;) {
    next = (write + 1) & ring.mask;
    if (next == ring.header->readIndex.load(std::memory_order_acquire)) {
      return false;  // full
    }
    if (ring.header->writeIndex.compare_exchange_weak(write, next,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
      break;
    }
    // compare_exchange_weak reloaded `write` with the winner's value; retry from there.
  }
  // Copy with ready forced clear: `entry` may itself have been popped from a ring and
  // still carry ready=1, and copying that in would publish the slot before the payload
  // bytes are all written.
  EventEntry staged = entry;
  staged.ready = 0;
  ring.entries[write] = staged;
  storeReady(ring.entries[write].ready, 1);
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
  if (loadReady(ring.entries[read].ready) == 0) {
    // Reserved, not yet published. Reading it now would hand on a half-written entry,
    // and skipping it would reorder this producer's commands, so report "nothing to
    // read" and come back. ringStalledSlot() distinguishes this from a genuinely
    // empty ring for the one case that cannot resolve itself: a dead producer.
    return false;
  }
  entry = ring.entries[read];
  return true;
}

bool ringStalledSlot(const EventRingView& ring, uint32_t& slotOut) {
  if (!ring.header || ring.mask == 0) {
    return false;
  }
  const uint32_t read = ring.header->readIndex.load(std::memory_order_relaxed);
  const uint32_t write = ring.header->writeIndex.load(std::memory_order_acquire);
  if (read == write || loadReady(ring.entries[read].ready) != 0) {
    return false;
  }
  slotOut = read;
  return true;
}

void ringSkipStalledSlot(EventRingView& ring) {
  if (!ring.header || ring.mask == 0) {
    return;
  }
  const uint32_t read = ring.header->readIndex.load(std::memory_order_relaxed);
  storeReady(ring.entries[read].ready, 0);
  ring.header->readIndex.store((read + 1) & ring.mask, std::memory_order_release);
}

bool ringPop(EventRingView& ring, EventEntry& entry) {
  if (!ringPeek(ring, entry)) {
    return false;
  }
  const uint32_t read = ring.header->readIndex.load(std::memory_order_relaxed);
  // Clear before releasing the slot, or the next producer to wrap onto it inherits a
  // ready flag from the previous lap and the consumer reads the slot mid-write.
  storeReady(ring.entries[read].ready, 0);
  ring.header->readIndex.store((read + 1) & ring.mask, std::memory_order_release);
  return true;
}

bool uiEditRingPop(UiEditRingView& ring, UiEditBatchEntry& entry) {
  if (!ring.header || ring.mask == 0) {
    return false;
  }
  const uint32_t read = ring.header->readIndex.load(std::memory_order_relaxed);
  const uint32_t write = ring.header->writeIndex.load(std::memory_order_acquire);
  if (read == write) {
    return false;
  }
  entry = ring.entries[read];
  ring.header->readIndex.store((read + 1) & ring.mask,
                               std::memory_order_release);
  return true;
}

}  // namespace daw
