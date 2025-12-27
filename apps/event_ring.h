#pragma once

#include <cstdint>

#include "apps/shared_memory.h"

namespace daw {

struct EventRingView {
  RingHeader* header = nullptr;
  EventEntry* entries = nullptr;
  uint32_t mask = 0;
};

EventRingView makeEventRing(void* base, uint64_t offset);
bool ringWrite(EventRingView& ring, const EventEntry& entry);
bool ringPeek(const EventRingView& ring, EventEntry& entry);
bool ringPop(EventRingView& ring, EventEntry& entry);

}  // namespace daw
