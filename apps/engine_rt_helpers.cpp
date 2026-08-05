// Bodies for apps/engine_rt_helpers.h. The WHY for each rule is in the header, beside the
// declaration; this file is the mechanics only.
#include "apps/engine_rt_helpers.h"

#include <limits>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
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
  // MEMBERSHIP IS DECIDED BY FLOOR; THE POSITION IS ROUNDED AND THEN CLAMPED. Two different
  // questions were being answered by one number, and that cost a note.
  //
  // tickDeltaToSamples ROUNDS, deliberately — truncating loses up to a sample per event and the
  // error accumulates across a block. But a nanotick is far smaller than a sample (0.0229688 at
  // 120 bpm / 44.1 kHz / Q=960000), so a tick INSIDE this block's window can round UP to exactly
  // blockSize: tickDelta 22290 of a 22291-tick window is 511.973 samples and rounds to 512. This
  // returned nothing for it, the caller skipped it, and the next block never emitted it either
  // because its tick window starts after that tick. The event simply vanished — 21 of every 22291
  // in-window positions, which reads as "a note goes missing sometimes".
  //
  // THE PATCHER PATH ALREADY FIXED THIS BY CLAMPING and its comment says, in as many words, that
  // "the CLIP path converts ticks to samples by a different route that has not been audited for
  // the same property". It had not. This is that audit: two paths that agreed on intent and
  // differed in behaviour, which is the shape this codebase keeps paying for.
  //
  // REJECTION SURVIVES, AND IT MUST. An event genuinely in a LATER block is still nothing —
  // clamping those would bunch every future event onto the boundary and play them at once, which
  // is what the callers' `continue` is for. floor() is what separates the two: if the event's own
  // sample lies past this block however it is rounded, it is not this block's event.
  const long double exact = static_cast<long double>(tickDelta) * samplesPerTick;
  if (exact < 0.0L || std::floor(exact) >= static_cast<long double>(blockSize)) {
    return std::nullopt;
  }
  uint64_t offset = tickDeltaToSamples(tickDelta, samplesPerTick);
  if (offset >= blockSize) {
    offset = blockSize - 1;  // half a sample early is inaudible; a missing note is not
  }
  return BlockPlacement{blockSampleStart + offset, static_cast<uint32_t>(offset)};
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

std::optional<uint64_t> audioChannelOffset(uint64_t planeBase, uint64_t strideBytes,
                                           uint32_t planeChannels, uint32_t blockIndex,
                                           uint32_t numBlocks, uint32_t channel,
                                           uint64_t shmSize) {
  if (strideBytes == 0 || planeChannels == 0 || numBlocks == 0 || shmSize == 0) {
    return std::nullopt;
  }
  const uint64_t blockBytes = static_cast<uint64_t>(planeChannels) * strideBytes;
  const uint64_t block = static_cast<uint64_t>(blockIndex % numBlocks);
  const uint64_t offset =
      planeBase + block * blockBytes + static_cast<uint64_t>(channel) * strideBytes;
  if (offset + strideBytes > shmSize) {
    return std::nullopt;
  }
  return offset;
}

void registerActiveNote(TrackRuntime& runtime, uint32_t noteId, uint8_t pitch, uint8_t column,
                        uint64_t startTick, uint64_t endTick, float tuningCents,
                        bool hasScheduledEnd) {
  ActiveNote activeNote;
  activeNote.noteId = noteId;
  activeNote.pitch = pitch;
  activeNote.column = column;
  activeNote.startNanotick = startTick;
  activeNote.endNanotick = endTick;
  activeNote.tuningCents = tuningCents;
  activeNote.hasScheduledEnd = hasScheduledEnd;
  runtime.activeNotes[noteId] = activeNote;
  runtime.activeNoteByColumn[column].push_back(noteId);
}

float modLinkValue(float bias, float depth, float sourceValue) {
  return clamp01(bias + depth * sourceValue);
}

bool modLinkCanFire(const daw::ModLink& link, bool srcPresent, bool dstPresent,
                    size_t srcPos, size_t dstPos) {
  if (!link.enabled || link.rate != daw::ModRate::BlockRate) {
    return false;
  }
  if (!srcPresent || !dstPresent) {
    return false;
  }
  // The source must be STRICTLY UPSTREAM of the target. Equal positions mean a device
  // modulating itself; greater means reading downstream, which within one block is last
  // block's value pretending to be this one's.
  if (srcPos >= dstPos) {
    return false;
  }
  return link.target.kind == daw::ModTargetKind::VstParam;
}

void applyBlockRateModulation(BlockModCtx& ctx) {
  std::vector<daw::ModSourceState> modSources;
  {
    std::lock_guard<std::mutex> lock(ctx.runtime.modSourcesMutex);
    modSources = ctx.runtime.modSources;
  }
  if (ctx.trackState.modLinks.empty() || modSources.empty()) {
    return;
  }
  const auto& chainDevices = ctx.trackState.chainDevices;
  std::unordered_map<uint32_t, size_t> chainPos;
  chainPos.reserve(chainDevices.size());
  for (size_t i = 0; i < chainDevices.size(); ++i) {
    chainPos.emplace(chainDevices[i].id, i);
  }
  auto findSourceValue = [&](const daw::ModSourceRef& source) -> std::optional<float> {
    for (const auto& state : modSources) {
      if (state.ref.deviceId == source.deviceId && state.ref.sourceId == source.sourceId &&
          state.ref.kind == source.kind) {
        return state.value;
      }
    }
    return std::nullopt;
  };
  auto resolveHostIndexForDevice = [&](uint32_t deviceId) -> std::optional<uint32_t> {
    uint32_t hostIndex = 0;
    for (const auto& device : chainDevices) {
      if (device.kind != daw::DeviceKind::VstInstrument &&
          device.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      if (!ctx.resolveDevicePluginPath(ctx.runtime, device.hostSlotIndex)) {
        continue;
      }
      if (device.id == deviceId) {
        return hostIndex;
      }
      ++hostIndex;
    }
    return std::nullopt;
  };
  for (const auto& link : ctx.trackState.modLinks) {
    const auto srcPos = chainPos.find(link.source.deviceId);
    const auto dstPos = chainPos.find(link.target.deviceId);
    const bool srcPresent = srcPos != chainPos.end();
    const bool dstPresent = dstPos != chainPos.end();
    if (!modLinkCanFire(link, srcPresent, dstPresent,
                        srcPresent ? srcPos->second : 0,
                        dstPresent ? dstPos->second : 0)) {
      continue;
    }
    const auto sourceValue = findSourceValue(link.source);
    if (!sourceValue) {
      continue;
    }
    const auto hostIndex = resolveHostIndexForDevice(link.target.deviceId);
    if (!hostIndex) {
      continue;
    }
    daw::EventEntry paramEntry;
    paramEntry.sampleTime = ctx.blockSampleStart;
    paramEntry.blockId = 0;
    paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
    paramEntry.size = sizeof(daw::ParamPayload);
    daw::ParamPayload payload{};
    std::memcpy(payload.uid16, link.target.uid16, sizeof(payload.uid16));
    payload.value = modLinkValue(link.bias, link.depth, *sourceValue);
    payload.targetPluginIndex = *hostIndex;
    std::memcpy(paramEntry.payload, &payload, sizeof(payload));
    pushScratchpad(ctx.scratch, paramEntry, ctx.windowStartTicks);
  }
}

uint8_t priorityForEvent(const daw::EventEntry& entry) {
  const auto type = static_cast<daw::EventType>(entry.type);
  switch (type) {
    case daw::EventType::Transport:
      return 0;
    case daw::EventType::Param:
      return 1;
    case daw::EventType::Midi: {
      daw::MidiPayload payload{};
      std::memcpy(&payload, entry.payload, sizeof(payload));
      if (payload.status == 0x80) {
        return 2;
      }
      if (payload.status == 0x90) {
        if (entry.flags & kEventFlagMusicalLogic) {
          return 3;
        }
        return 4;
      }
      return 4;
    }
    case daw::EventType::MusicalLogic:
      return 3;
    default:
      return 4;
  }
}

uint8_t resolvedBaseOctave(uint8_t hint, int32_t octaveOffset) {
  const uint8_t base = hint != 0 ? hint : 4;
  int v = static_cast<int>(base) + static_cast<int>(octaveOffset);
  if (v < 0) {
    v = 0;
  } else if (v > 10) {
    v = 10;
  }
  return static_cast<uint8_t>(v);
}

uint8_t resolvedVelocity(uint8_t velocity) {
  return velocity != 0 ? velocity : 100;
}


namespace {
// A loop whose end is not past its start is no loop at all.
inline uint64_t loopLengthOf(uint64_t loopStartTick, uint64_t loopEndTick) {
  return loopEndTick > loopStartTick ? loopEndTick - loopStartTick : 0;
}
}  // namespace

uint64_t advanceTransportTick(uint64_t nextTick, uint64_t loopStartTick, uint64_t loopEndTick) {
  const uint64_t loopLen = loopLengthOf(loopStartTick, loopEndTick);
  if (loopLen > 0 && nextTick >= loopEndTick) {
    return loopStartTick + ((nextTick - loopStartTick) % loopLen);
  }
  return nextTick;
}

uint64_t wrapTickIntoLoop(uint64_t tick, uint64_t loopStartTick, uint64_t loopEndTick) {
  const uint64_t loopLen = loopLengthOf(loopStartTick, loopEndTick);
  if (loopLen == 0) {
    return tick;
  }
  if (tick < loopStartTick) {
    return loopStartTick;
  }
  if (tick >= loopEndTick) {
    return loopStartTick + ((tick - loopStartTick) % loopLen);
  }
  return tick;
}

LoopSplit splitWindowAtLoopEnd(uint64_t windowStart, uint64_t windowEnd,
                               uint64_t loopStartTick, uint64_t loopEndTick) {
  LoopSplit out;
  const uint64_t loopLen = loopLengthOf(loopStartTick, loopEndTick);
  if (loopLen == 0 || windowEnd <= loopEndTick) {
    out.segments[0] = LoopSegment{windowStart, windowEnd, 0};
    out.count = 1;
    return out;
  }
  const uint64_t firstLen = loopEndTick - windowStart;
  out.segments[0] = LoopSegment{windowStart, loopEndTick, 0};
  out.segments[1] =
      LoopSegment{loopStartTick, loopStartTick + (windowEnd - loopEndTick), firstLen};
  out.count = 2;
  return out;
}


LoopBounds effectiveLoop(uint64_t loopStartTick, uint64_t loopEndTick, uint64_t patternTicks) {
  if (loopEndTick <= loopStartTick) {
    return LoopBounds{0, patternTicks};
  }
  return LoopBounds{loopStartTick, loopEndTick};
}

uint64_t clampTickIntoLoop(uint64_t tick, uint64_t loopStartTick, uint64_t loopEndTick) {
  uint64_t clamped = tick < loopStartTick ? loopStartTick : tick;
  if (loopEndTick > 0 && clamped >= loopEndTick) {
    clamped = loopEndTick - 1;
  }
  return clamped;
}


CompletedMinimum completedMinimum(const std::vector<HostProgress>& hosts, uint32_t nextBlockId) {
  CompletedMinimum out;
  uint32_t lowest = std::numeric_limits<uint32_t>::max();
  // TWO DIFFERENT QUESTIONS, and merging them starved the producer almost completely.
  // `anyContributing` answers "is any host producing at all" — the caller turns its negation into
  // throttleInactive, which makes produceBlock SLEEP A WHOLE BLOCK every iteration (that is the
  // pacing for an all-sampler project, which has no hosts). `anyOwing` answers "does anyone still
  // owe me a block", which is the only question back-pressure should ask. A first version of this
  // excluded owes-nothing hosts from BOTH, so in ordinary steady state — every host having
  // finished exactly what it was sent — anyContributing went false and the producer slept itself
  // to death: `panic` went from "no block ready for 509 of 1469 callbacks" to 1463 of 1468.
  bool anyOwing = false;
  for (const auto& h : hosts) {
    if (!h.active) {
      continue;
    }
    if (h.completedBlockId == 0) {
      continue;   // attached but has finished nothing yet — see the header
    }
    // It exists and it is producing. That is true whether or not it currently owes anything, and
    // it is what keeps the throttle off.
    out.anyContributing = true;
    if (h.lastDispatchedBlockId > 0 && h.completedBlockId >= h.lastDispatchedBlockId) {
      // OWES NOTHING, so it cannot advance without new work — gating on it deadlocks. See the
      // header: this is the skipped-for-dispatch case (a VST load holds the track's
      // controllerMutex, produce_block try_locks it and returns), where the host rejoins with a
      // stale id it can never improve on because the closed gate is what stops the dispatch.
      continue;
    }
    anyOwing = true;
    lowest = std::min(lowest, h.completedBlockId);
  }
  // Nobody owing is not the same as nobody producing: the producer is free to make the next
  // block, so the minimum is the same one-behind fallback used when there are no hosts at all.
  out.minCompleted = anyOwing
                         ? lowest
                         : (nextBlockId > 0 ? nextBlockId - 1 : 0);
  return out;
}

}  // namespace daw::engine
