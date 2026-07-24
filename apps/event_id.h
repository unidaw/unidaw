#pragma once

#include <cstdint>

namespace daw {

// Identity for a musical event, stable for the life of the object.
//
// Layout: author in the top 16 bits, a per-author counter in the low 48.
// Two properties follow, and both matter:
//
//   * Collisions are impossible by construction rather than by coordination.
//     The previous scheme was a per-clip uint32 counter starting at 1, so the
//     first note of every clip in a project had id 1 — ids were not unique
//     even within one document, which makes them useless for naming anything.
//
//   * Provenance is a mask, not a side table. "Which notes did the agent
//     write" is a bit test on the id, so an edit made by a second author can
//     be found, listed and reverted without storing that fact anywhere else.
//
// 48 bits of counter is ~281 trillion events per author; a session that
// allocated one per microsecond would take nine years to wrap.
using EventId = uint64_t;

constexpr uint16_t kAuthorHuman = 0;
constexpr uint16_t kAuthorAgent = 1;
constexpr uint16_t kAuthorImport = 2;

constexpr uint64_t kEventIdCounterMask = 0x0000'FFFF'FFFF'FFFFull;
constexpr int kEventIdAuthorShift = 48;

constexpr EventId makeEventId(uint16_t author, uint64_t counter) {
  return (static_cast<uint64_t>(author) << kEventIdAuthorShift) |
         (counter & kEventIdCounterMask);
}

constexpr uint16_t eventIdAuthor(EventId id) {
  return static_cast<uint16_t>(id >> kEventIdAuthorShift);
}

constexpr uint64_t eventIdCounter(EventId id) {
  return id & kEventIdCounterMask;
}

// Id 0 means "not yet assigned"; the allocator never returns it.
constexpr EventId kEventIdNone = 0;

}  // namespace daw
