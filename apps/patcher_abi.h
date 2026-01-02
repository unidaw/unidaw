#pragma once

#include <atomic>
#include <cstdint>

#include "apps/harmony_timeline.h"
#include "apps/shared_memory.h"

namespace daw {

constexpr uint16_t kEventTypeMusicalLogic = 9;
constexpr uint8_t kMusicalLogicKindGate = 1;
constexpr uint8_t kMusicalLogicKindDegree = 2;

struct MusicalLogicPayload {
  uint8_t degree = 0;
  int8_t octave_offset = 0;
  uint8_t _pad0[2]{};
  uint32_t chord_id = 0;
  uint64_t duration_ticks = 0;
  uint8_t priority_hint = 0;
  uint8_t velocity = 0;
  uint8_t base_octave = 0;
  uint8_t metadata[21]{};
};

struct PatcherEuclideanConfig {
  uint32_t steps = 16;
  uint32_t hits = 5;
  uint32_t offset = 0;
  uint64_t duration_ticks = 0;
  uint8_t degree = 1;
  int8_t octave_offset = 0;
  uint8_t velocity = 100;
  uint8_t base_octave = 4;
  uint8_t _pad0[2]{};
};

struct PatcherRandomDegreeConfig {
  uint8_t degree = 8;
  uint8_t velocity = 100;
  uint8_t _pad0[2]{};
  uint64_t duration_ticks = 0;
};

struct PatcherLfoConfig {
  float frequency_hz = 1.0f;
  float depth = 1.0f;
  float bias = 0.0f;
  float phase_offset = 0.0f;
};

struct alignas(64) PatcherContext {
  uint32_t abi_version = 2;
  uint64_t block_start_tick = 0;
  uint64_t block_end_tick = 0;
  float sample_rate = 0.0f;
  float tempo_bpm = 120.0f;
  uint32_t num_frames = 0;

  EventEntry* event_buffer = nullptr;
  uint32_t event_capacity = 0;
  uint32_t* event_count = nullptr;
  uint64_t* last_overflow_tick = nullptr;

  float** audio_channels = nullptr;
  uint32_t num_channels = 0;

  const void* node_config = nullptr;
  uint32_t node_config_size = 0;

  const HarmonyEvent* harmony_snapshot = nullptr;
  uint32_t harmony_count = 0;

  float* mod_outputs = nullptr;
  uint32_t mod_output_count = 0;
  float* mod_output_samples = nullptr;
  uint32_t mod_output_stride = 0;

  float* mod_inputs = nullptr;
  uint32_t mod_input_count = 0;
  uint32_t mod_input_stride = 0;
};

#if defined(__GNUC__) || defined(__clang__)
#define DAW_WEAK __attribute__((weak))
#else
#define DAW_WEAK
#endif

extern "C" void atomic_store_u64(uint64_t* ptr, uint64_t value);
extern "C" void patcher_process(PatcherContext* ctx) DAW_WEAK;
extern "C" void patcher_process_euclidean(PatcherContext* ctx) DAW_WEAK;
extern "C" void patcher_process_random_degree(PatcherContext* ctx) DAW_WEAK;
extern "C" void patcher_process_event_out(PatcherContext* ctx) DAW_WEAK;
extern "C" void patcher_process_lfo(PatcherContext* ctx) DAW_WEAK;
extern "C" void patcher_process_passthrough(PatcherContext* ctx) DAW_WEAK;
extern "C" void patcher_process_audio_passthrough(PatcherContext* ctx) DAW_WEAK;
#undef DAW_WEAK

static_assert(sizeof(EventEntry) == 64, "EventEntry size mismatch");
static_assert(alignof(EventEntry) == 64, "EventEntry alignment mismatch");
static_assert(sizeof(MusicalLogicPayload) <= 40,
              "MusicalLogicPayload exceeds EventEntry payload");
static_assert(sizeof(PatcherLfoConfig) == 16, "PatcherLfoConfig size mismatch");

}  // namespace daw
