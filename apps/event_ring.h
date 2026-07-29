#pragma once

#include <cstdint>

#include "apps/shared_memory.h"

namespace daw {

struct EventRingView {
  RingHeader* header = nullptr;
  EventEntry* entries = nullptr;
  uint32_t mask = 0;
};

struct UiEditRingView {
  RingHeader* header = nullptr;
  UiEditBatchEntry* entries = nullptr;
  uint32_t mask = 0;
};

EventRingView makeEventRing(void* base, uint64_t offset);
UiEditRingView makeUiEditRing(void* base, uint64_t offset);
bool ringWrite(EventRingView& ring, const EventEntry& entry);
bool ringPeek(const EventRingView& ring, EventEntry& entry);
bool ringPop(EventRingView& ring, EventEntry& entry);
// M2.18: true when the ring is non-empty but the slot at readIndex has been reserved
// and not yet published. Normally that resolves within a few instructions; it only
// persists if the producer died between reserving and publishing, which is the one
// case the consumer cannot wait out. Callers watch it for a while and then call
// ringSkipStalledSlot() to retire the abandoned slot.
bool ringStalledSlot(const EventRingView& ring, uint32_t& slotOut);
void ringSkipStalledSlot(EventRingView& ring);
bool uiEditRingPop(UiEditRingView& ring, UiEditBatchEntry& entry);

}  // namespace daw
