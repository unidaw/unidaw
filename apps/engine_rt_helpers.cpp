// Bodies for apps/engine_rt_helpers.h. The WHY for each rule is in the header, beside the
// declaration; this file is the mechanics only.
#include "apps/engine_rt_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace daw::engine {

std::optional<daw::HarmonyEvent> harmonyAtOrDefault(
    const std::vector<daw::HarmonyEvent>& events, uint64_t nanotick) {
  if (events.empty()) {
    return daw::HarmonyEvent{0, 0, 1, 0};
  }
  return daw::harmonyAt(events, nanotick);
}

daw::ResolvedPitch quantizePitch(const daw::ScaleRegistry& registry, uint8_t pitch,
                                 const daw::HarmonyEvent& harmony) {
  const auto* scale = registry.find(harmony.scaleId);
  if (!scale) {
    return daw::resolvedPitchFromCents(static_cast<double>(pitch) * 100.0);
  }
  return daw::quantizeToScale(pitch, harmony.root, *scale);
}

void enqueueMirrorReplay(TrackRuntime& runtime) {
  if (runtime.isAuxChild.load(std::memory_order_acquire)) {
    return;
  }
  runtime.mirrorGateSampleTime.store(0, std::memory_order_release);
  runtime.mirrorPending.store(true, std::memory_order_release);
  runtime.mirrorPrimed.store(false, std::memory_order_release);
}

uint64_t tickDeltaToSamples(uint64_t tickDelta, long double samplesPerTick) {
  return static_cast<uint64_t>(
      std::llround(static_cast<long double>(tickDelta) * samplesPerTick));
}

float clamp01(float value) {
  return std::max(0.0f, std::min(1.0f, value));
}

uint8_t rampedVelocity(uint8_t velocity, uint16_t scaleMilli) {
  if (scaleMilli == 1000) {
    return velocity;
  }
  const uint32_t scaled =
      (static_cast<uint32_t>(velocity) * scaleMilli + 500u) / 1000u;
  // Floor of 1, not 0: velocity 0 is a note-off in MIDI, so a ramp that reached
  // zero would not be a silent strike but a stuck one.
  return static_cast<uint8_t>(scaled < 1 ? 1 : (scaled > 127 ? 127 : scaled));
}

std::optional<uint32_t> nodeIndexForId(const daw::PatcherGraph& graph, uint32_t nodeId) {
  if (nodeId >= graph.idToIndex.size()) {
    return std::nullopt;
  }
  const uint32_t index = graph.idToIndex[nodeId];
  if (index == daw::kPatcherInvalidNodeIndex) {
    return std::nullopt;
  }
  return index;
}

void removeNoteIdFromColumn(TrackRuntime& runtime, uint8_t column, uint32_t noteId) {
  auto columnIt = runtime.activeNoteByColumn.find(column);
  if (columnIt == runtime.activeNoteByColumn.end()) {
    return;
  }
  auto& notes = columnIt->second;
  notes.erase(std::remove(notes.begin(), notes.end(), noteId), notes.end());
  if (notes.empty()) {
    runtime.activeNoteByColumn.erase(columnIt);
  }
}

daw::EventEntry makeNoteOffEntry(uint64_t sampleTime, uint32_t blockId, uint8_t pitch,
                                 uint8_t channel, int16_t tuningCents, uint32_t noteId,
                                 uint32_t flags) {
  daw::EventEntry entry;
  entry.sampleTime = sampleTime;
  entry.blockId = blockId;
  entry.type = static_cast<uint16_t>(daw::EventType::Midi);
  entry.size = sizeof(daw::MidiPayload);
  entry.flags = flags;
  daw::MidiPayload off{};
  off.status = 0x80;
  off.data1 = pitch;
  off.data2 = 0;
  off.channel = channel;
  off.tuningCents = tuningCents;
  off.noteId = noteId;
  std::memcpy(entry.payload, &off, sizeof(off));
  return entry;
}

daw::SamplerEvent samplerNoteOffFor(const daw::EventEntry& noteOff, uint64_t blockSampleStart,
                                    uint32_t blockSize, uint32_t noteId) {
  daw::SamplerEvent se;
  const int64_t off =
      static_cast<int64_t>(noteOff.sampleTime) - static_cast<int64_t>(blockSampleStart);
  se.offsetInBlock = static_cast<uint32_t>(
      off < 0 ? 0 : (off >= static_cast<int64_t>(blockSize) ? blockSize - 1 : off));
  se.kind = daw::SamplerEventKind::NoteOff;
  se.noteId = noteId;
  return se;
}

}  // namespace daw::engine
