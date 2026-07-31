#pragma once

#include <atomic>
#include <cstdint>

#include "apps/harmony_timeline.h"
#include "apps/shared_memory.h"

namespace daw {

// The patcher node ABI. A node function refuses to run unless the context declares
// exactly this version, so ANY caller that hardcodes a number silently stops executing
// nodes the moment it is bumped — which is what happened to patcher_graph_tests, pinned
// at 3 while this went to 4, quietly turning its whole node-execution harness into a
// no-op. Everything that fills a PatcherContext uses this constant.
constexpr uint32_t kPatcherAbiVersion = 4;


constexpr uint16_t kEventTypeMusicalLogic = 9;
constexpr uint8_t kMusicalLogicKindGate = 1;
constexpr uint8_t kMusicalLogicKindDegree = 2;

struct MusicalLogicPayload {
  uint8_t degree = 0;
  int8_t octave_offset = 0;
  // WHICH SOUND THIS NOTE PLAYS, or 0 for "no address — let the keymap pick from the pitch".
  //
  // This was `uint8_t _pad0[2]`, at the same offset and the same size, zeroed on both sides and
  // read by neither. Naming it costs no layout change and no PATCHER_ABI_VERSION bump: an old
  // patcher_rust writes 0 and a new engine reads 0, which is today's behaviour, and a new
  // patcher_rust writing a value into an old engine is ignored, which is also today's behaviour.
  // A version gate here would only refuse combinations that work.
  //
  // The sampler's sound address has been a per-NOTE field since v32 (UiClipNote.sound), so a
  // clip could always say which slice to play and a GENERATED note could not — it fell back to
  // the keymap whatever the graph did. This is the field SliceSelect writes.
  uint16_t sound = 0;
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

// SliceSelect: which SOUND a generated note plays.
//
// `count` is the SIZE of the range, so 0 and 1 both mean "always `base`" — a range being empty
// rather than a sentinel being decoded, exactly as PatcherRandomDegreeConfig::degree works.
// `base` is the first sound address in the range: a chop laid down from slot 1 is base=1,
// count=8. A base of 0 is clamped to 1, because 0 is the sound address that MEANS "no address,
// let the keymap pick" — a node configured with it would look set up and do nothing.
struct PatcherSliceSelectConfig {
  uint16_t base = 1;
  uint16_t count = 8;
  uint8_t reserved[4]{};
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
  uint32_t abi_version = kPatcherAbiVersion;
  uint64_t block_start_tick = 0;
  uint64_t block_end_tick = 0;
  uint64_t block_start_sample = 0;
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

  // ABI 4 — REPRODUCIBLE GENERATION. A generator must produce the same music every time
  // for the same project, and different music per node: without node_id two random_degree
  // nodes at the same musical tick generate the IDENTICAL pitch, because the tick was the
  // whole seed. `seed` is the project's stored seed, so a variation can be re-rolled by
  // changing one number and everything else stays put. Appended at the end so the flat C
  // layout of every earlier field is untouched.
  uint32_t node_id = 0;
  uint64_t seed = 0;
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
extern "C" void patcher_process_slice_select(PatcherContext* ctx) DAW_WEAK;
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
// HarmonyEvent crosses this ABI too — PatcherContext hands a `const HarmonyEvent*` to
// the Rust nodes, which mirror it #[repr(C)] in patcher_rust/src/lib.rs — and it was the
// ONE struct on the wire with nothing guarding it. Changing its shape or a field width
// would compile clean on both sides and every generator would read garbage roots and
// scales at runtime, on the producer thread. Found by an adversarial design review that
// proposed exactly that change without noticing.
static_assert(sizeof(HarmonyEvent) == 24, "HarmonyEvent size mismatch (crosses the ABI)");
static_assert(alignof(HarmonyEvent) == 8, "HarmonyEvent alignment mismatch");
static_assert(offsetof(HarmonyEvent, nanotick) == 0, "HarmonyEvent::nanotick moved");
static_assert(offsetof(HarmonyEvent, root) == 8, "HarmonyEvent::root moved");
static_assert(offsetof(HarmonyEvent, scaleId) == 12, "HarmonyEvent::scaleId moved");
static_assert(offsetof(HarmonyEvent, flags) == 16, "HarmonyEvent::flags moved");

}  // namespace daw
