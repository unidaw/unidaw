// Bodies for apps/engine_rt_helpers.h. The WHY for each rule is in the header, beside the
// declaration; this file is the mechanics only.
#include "apps/engine_rt_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

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

bool pushScratchpad(NoteCutCtx& ctx, const daw::EventEntry& entry, uint64_t overflowTick) {
  auto& scratchpad = ctx.runtime.patcherScratchpad;
  if (ctx.scratchpadCount < scratchpad.size()) {
    scratchpad[ctx.scratchpadCount++] = entry;
    return true;
  }
  daw::atomic_store_u64(reinterpret_cast<uint64_t*>(&ctx.lastOverflowTick), overflowTick);
  return false;
}

void cutActiveNotes(NoteCutCtx& ctx, uint64_t eventSample, std::optional<uint8_t> column) {
  auto& runtime = ctx.runtime;
  std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
  if (runtime.activeNotes.empty()) {
    return;
  }
  std::vector<uint32_t> noteIds;
  noteIds.reserve(runtime.activeNotes.size());
  for (const auto& [noteId, activeNote] : runtime.activeNotes) {
    if (!column || activeNote.column == *column) {
      noteIds.push_back(noteId);
    }
  }
  for (uint32_t noteId : noteIds) {
    auto noteIt = runtime.activeNotes.find(noteId);
    if (noteIt == runtime.activeNotes.end()) {
      continue;
    }
    const ActiveNote activeNote = noteIt->second;
    const daw::EventEntry noteOffEntry =
        makeNoteOffEntry(eventSample, 0, activeNote.pitch, ctx.midiChannel,
                         activeNote.tuningCents, activeNote.noteId);
    pushScratchpad(ctx, noteOffEntry, activeNote.endNanotick);
    if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
      runtime.samplerEvents.push_back(samplerNoteOffFor(
          noteOffEntry, ctx.blockSampleStart, ctx.blockSize, activeNote.noteId));
    }
    runtime.activeNotes.erase(noteIt);
    removeNoteIdFromColumn(runtime, activeNote.column, noteId);
  }
}

daw::EventEntry makeNoteOnEntry(uint64_t sampleTime, uint32_t blockId, uint8_t pitch,
                                uint8_t velocity, uint8_t channel, float tuningCents,
                                uint32_t noteId, uint32_t flags) {
  daw::EventEntry entry;
  entry.sampleTime = sampleTime;
  entry.blockId = blockId;
  entry.type = static_cast<uint16_t>(daw::EventType::Midi);
  entry.size = sizeof(daw::MidiPayload);
  entry.flags = flags;
  daw::MidiPayload on{};
  on.status = 0x90;
  on.data1 = pitch;
  on.data2 = velocity;
  on.channel = channel;
  on.tuningCents = tuningCents;
  on.noteId = noteId;
  std::memcpy(entry.payload, &on, sizeof(on));
  return entry;
}

daw::SamplerEvent samplerNoteOnFor(uint32_t offsetInBlock, uint8_t pitch, uint8_t velocity,
                                   uint8_t column, uint16_t sound, uint16_t offsetFrac,
                                   bool soundAddressedOnly, uint32_t noteId) {
  daw::SamplerEvent se;
  se.offsetInBlock = offsetInBlock;
  se.kind = daw::SamplerEventKind::NoteOn;
  se.pitch = pitch;
  se.velocity = velocity;
  se.column = column;
  se.sound = sound;
  se.soundAddressedOnly = soundAddressedOnly;
  se.offsetFrac = offsetFrac;
  se.noteId = noteId;
  return se;
}

std::optional<BlockPlacement> placeInBlock(uint64_t tickDelta, uint64_t blockSampleStart,
                                           long double samplesPerTick, uint32_t blockSize) {
  const uint64_t sampleTime = blockSampleStart + tickDeltaToSamples(tickDelta, samplesPerTick);
  const int64_t offset =
      static_cast<int64_t>(sampleTime) - static_cast<int64_t>(blockSampleStart);
  if (offset < 0 || offset >= static_cast<int64_t>(blockSize)) {
    return std::nullopt;
  }
  return BlockPlacement{sampleTime, static_cast<uint32_t>(offset)};
}

void queuePendingStrikes(TrackRuntime& runtime, const std::vector<PendingStrike>& strikes) {
  if (strikes.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
  for (const auto& q : strikes) {
    bool exists = false;
    for (const auto& ps : runtime.pendingStrikes) {
      if (ps.onTick == q.onTick && ps.pitch == q.pitch && ps.column == q.column) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      runtime.pendingStrikes.push_back(q);
    }
  }
}

}  // namespace daw::engine
