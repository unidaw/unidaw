#pragma once

#include <atomic>
#include <cstdint>

#include "apps/harmony_timeline.h"
#include "apps/shared_memory.h"

namespace daw {

constexpr uint16_t kEventTypeMusicalLogic = 9;

struct MusicalLogicPayload {
  uint8_t degree = 0;
  int8_t octave_offset = 0;
  uint8_t _pad0[2]{};
  uint32_t chord_id = 0;
  uint64_t duration_ticks = 0;
  uint8_t priority_hint = 0;
  uint8_t metadata[23]{};
};

struct alignas(64) PatcherContext {
  uint32_t abi_version = 1;
  uint64_t block_start_tick = 0;
  uint64_t block_end_tick = 0;
  float sample_rate = 0.0f;

  EventEntry* event_buffer = nullptr;
  uint32_t event_capacity = 0;
  uint32_t* event_count = nullptr;
  uint64_t* last_overflow_tick = nullptr;

  const HarmonyEvent* harmony_snapshot = nullptr;
  uint32_t harmony_count = 0;
};

extern "C" void atomic_store_u64(uint64_t* ptr, uint64_t value);

static_assert(sizeof(EventEntry) == 64, "EventEntry size mismatch");
static_assert(alignof(EventEntry) == 64, "EventEntry alignment mismatch");
static_assert(sizeof(MusicalLogicPayload) <= 40,
              "MusicalLogicPayload exceeds EventEntry payload");

}  // namespace daw
