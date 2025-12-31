#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "apps/patcher_abi.h"
#include "apps/scale_library.h"
#include "apps/chord_resolver.h"

namespace {

struct MidiPayload {
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint8_t channel = 0;
  float tuningCents = 0.0f;
  uint32_t noteId = 0;
  uint8_t reserved[28]{};
};

uint8_t clampMidi(int pitch) {
  if (pitch < 0) {
    return 0;
  }
  if (pitch > 127) {
    return 127;
  }
  return static_cast<uint8_t>(pitch);
}

void resolveMusicalLogic(daw::EventEntry& entry,
                         uint64_t windowStartTicks,
                         uint64_t windowEndTicks,
                         const std::vector<daw::HarmonyEvent>& harmony,
                         std::vector<daw::EventEntry>& outNoteOffs) {
  if (static_cast<daw::EventType>(entry.type) != daw::EventType::MusicalLogic) {
    return;
  }
  daw::MusicalLogicPayload logic{};
  std::memcpy(&logic, entry.payload, sizeof(logic));
  const uint64_t eventTick = entry.sampleTime;
  auto harmonyAt = [&](uint64_t tick) -> std::optional<daw::HarmonyEvent> {
    return daw::harmonyAt(harmony, tick);
  };
  const auto harmonyEvent = harmonyAt(eventTick);
  if (!harmonyEvent) {
    return;
  }
  const auto* scale = daw::ScaleRegistry::instance().find(harmonyEvent->scaleId);
  if (!scale) {
    return;
  }
  const uint8_t rootPc = static_cast<uint8_t>(harmonyEvent->root % 12);
  const uint8_t baseOctave =
      logic.base_octave != 0 ? logic.base_octave : 4;
  const int baseOctaveInt =
      static_cast<int>(baseOctave) + static_cast<int>(logic.octave_offset);
  const uint8_t clampedBase =
      clampMidi(baseOctaveInt < 0 ? 0 : (baseOctaveInt > 10 ? 10 : baseOctaveInt));
  const auto resolved =
      daw::resolveDegree(logic.degree, clampedBase, rootPc, *scale);
  const uint8_t pitch = clampMidi(resolved.midi);
  const uint8_t velocity = logic.velocity != 0 ? logic.velocity : 100;

  MidiPayload on{};
  on.status = 0x90;
  on.data1 = pitch;
  on.data2 = velocity;
  on.channel = 0;
  on.tuningCents = resolved.cents;
  on.noteId = 1;
  entry.type = static_cast<uint16_t>(daw::EventType::Midi);
  entry.size = sizeof(MidiPayload);
  std::memcpy(entry.payload, &on, sizeof(on));

  if (logic.duration_ticks > 0) {
    const uint64_t offTick = eventTick + logic.duration_ticks;
    if (offTick >= windowStartTicks && offTick < windowEndTicks) {
      daw::EventEntry off{};
      off.sampleTime = offTick;
      off.type = static_cast<uint16_t>(daw::EventType::Midi);
      off.size = sizeof(MidiPayload);
      MidiPayload offPayload{};
      offPayload.status = 0x80;
      offPayload.data1 = pitch;
      offPayload.data2 = 0;
      offPayload.channel = 0;
      offPayload.tuningCents = resolved.cents;
      offPayload.noteId = 1;
      std::memcpy(off.payload, &offPayload, sizeof(offPayload));
      outNoteOffs.push_back(off);
    }
  }
}

uint8_t priorityFor(const daw::EventEntry& entry) {
  const auto type = static_cast<daw::EventType>(entry.type);
  switch (type) {
    case daw::EventType::Transport:
      return 0;
    case daw::EventType::Param:
      return 1;
    case daw::EventType::Midi: {
      MidiPayload payload{};
      std::memcpy(&payload, entry.payload, sizeof(payload));
      if (payload.status == 0x80) {
        return 2;
      }
      return 4;
    }
    case daw::EventType::MusicalLogic:
      return 3;
    default:
      return 4;
  }
}

uint8_t priorityHintFor(const daw::EventEntry& entry) {
  if (static_cast<daw::EventType>(entry.type) != daw::EventType::MusicalLogic) {
    return 0;
  }
  daw::MusicalLogicPayload logic{};
  std::memcpy(&logic, entry.payload, sizeof(logic));
  return logic.priority_hint;
}

}  // namespace

int main() {
  std::vector<daw::HarmonyEvent> harmony;
  harmony.push_back(daw::HarmonyEvent{0, 0, 1, 0});

  daw::EventEntry logicEntry{};
  logicEntry.sampleTime = 0;
  logicEntry.type = static_cast<uint16_t>(daw::EventType::MusicalLogic);
  logicEntry.size = sizeof(daw::MusicalLogicPayload);
  daw::MusicalLogicPayload logic{};
  logic.degree = 1;
  logic.duration_ticks = 480;
  logic.velocity = 90;
  logic.base_octave = 4;
  std::memcpy(logicEntry.payload, &logic, sizeof(logic));

  std::vector<daw::EventEntry> noteOffs;
  resolveMusicalLogic(logicEntry, 0, 960, harmony, noteOffs);

  assert(static_cast<daw::EventType>(logicEntry.type) == daw::EventType::Midi);
  MidiPayload on{};
  std::memcpy(&on, logicEntry.payload, sizeof(on));
  assert(on.status == 0x90);
  assert(on.data2 == 90);
  assert(noteOffs.size() == 1);
  MidiPayload off{};
  std::memcpy(&off, noteOffs[0].payload, sizeof(off));
  assert(off.status == 0x80);

  std::vector<daw::EventEntry> events;
  daw::EventEntry transport{};
  transport.sampleTime = 0;
  transport.type = static_cast<uint16_t>(daw::EventType::Transport);
  events.push_back(transport);

  daw::EventEntry param{};
  param.sampleTime = 0;
  param.type = static_cast<uint16_t>(daw::EventType::Param);
  events.push_back(param);

  daw::EventEntry noteOff{};
  noteOff.sampleTime = 0;
  noteOff.type = static_cast<uint16_t>(daw::EventType::Midi);
  MidiPayload noteOffPayload{};
  noteOffPayload.status = 0x80;
  std::memcpy(noteOff.payload, &noteOffPayload, sizeof(noteOffPayload));
  events.push_back(noteOff);

  daw::EventEntry logicEntry2{};
  logicEntry2.sampleTime = 0;
  logicEntry2.type = static_cast<uint16_t>(daw::EventType::MusicalLogic);
  daw::MusicalLogicPayload logic2{};
  logic2.priority_hint = 5;
  std::memcpy(logicEntry2.payload, &logic2, sizeof(logic2));
  events.push_back(logicEntry2);

  daw::EventEntry noteOn{};
  noteOn.sampleTime = 0;
  noteOn.type = static_cast<uint16_t>(daw::EventType::Midi);
  MidiPayload noteOnPayload{};
  noteOnPayload.status = 0x90;
  std::memcpy(noteOn.payload, &noteOnPayload, sizeof(noteOnPayload));
  events.push_back(noteOn);

  std::stable_sort(events.begin(), events.end(),
                   [&](const daw::EventEntry& a, const daw::EventEntry& b) {
                     const auto pa = priorityFor(a);
                     const auto pb = priorityFor(b);
                     const auto ha = priorityHintFor(a);
                     const auto hb = priorityHintFor(b);
                     return std::tie(a.sampleTime, pa, ha) <
                         std::tie(b.sampleTime, pb, hb);
                   });
  assert(static_cast<daw::EventType>(events[0].type) == daw::EventType::Transport);
  assert(static_cast<daw::EventType>(events[1].type) == daw::EventType::Param);
  assert(priorityFor(events[2]) == 2);
  assert(static_cast<daw::EventType>(events[3].type) == daw::EventType::MusicalLogic);
  assert(priorityFor(events[4]) == 4);

  uint64_t overflowTick = 0;
  auto pushWithOverflow = [&](uint32_t& count,
                              uint32_t capacity,
                              uint64_t tick) {
    if (count < capacity) {
      ++count;
    } else {
      overflowTick = tick;
    }
  };
  uint32_t scratchCount = 0;
  pushWithOverflow(scratchCount, 2, 10);
  pushWithOverflow(scratchCount, 2, 20);
  pushWithOverflow(scratchCount, 2, 30);
  assert(scratchCount == 2);
  assert(overflowTick == 30);

  std::vector<daw::EventEntry> nodeA(2);
  std::vector<daw::EventEntry> nodeB(2);
  nodeA[0].sampleTime = 0;
  nodeA[0].type = static_cast<uint16_t>(daw::EventType::Midi);
  nodeA[1].sampleTime = 0;
  nodeA[1].type = static_cast<uint16_t>(daw::EventType::Midi);
  nodeB[0].sampleTime = 0;
  nodeB[0].type = static_cast<uint16_t>(daw::EventType::Midi);
  nodeB[1].sampleTime = 0;
  nodeB[1].type = static_cast<uint16_t>(daw::EventType::Midi);
  MidiPayload tagA0{};
  tagA0.noteId = 10;
  std::memcpy(nodeA[0].payload, &tagA0, sizeof(tagA0));
  MidiPayload tagA1{};
  tagA1.noteId = 11;
  std::memcpy(nodeA[1].payload, &tagA1, sizeof(tagA1));
  MidiPayload tagB0{};
  tagB0.noteId = 20;
  std::memcpy(nodeB[0].payload, &tagB0, sizeof(tagB0));
  MidiPayload tagB1{};
  tagB1.noteId = 21;
  std::memcpy(nodeB[1].payload, &tagB1, sizeof(tagB1));

  std::vector<daw::EventEntry> merged1;
  std::vector<daw::EventEntry> merged2;
  for (const auto& entry : nodeA) {
    merged1.push_back(entry);
  }
  for (const auto& entry : nodeB) {
    merged1.push_back(entry);
  }
  for (const auto& entry : nodeB) {
    merged2.push_back(entry);
  }
  for (const auto& entry : nodeA) {
    merged2.push_back(entry);
  }
  assert(merged1.size() == merged2.size());
  std::vector<uint32_t> topoOrder = {0, 1};
  std::vector<std::vector<daw::EventEntry>> nodeBuffers = {nodeA, nodeB};
  std::vector<daw::EventEntry> mergedTopo;
  for (uint32_t nodeIndex : topoOrder) {
    for (const auto& entry : nodeBuffers[nodeIndex]) {
      mergedTopo.push_back(entry);
    }
  }
  assert(mergedTopo.size() == merged1.size());
  assert(mergedTopo.size() == merged2.size());
  MidiPayload mergedTag{};
  std::memcpy(&mergedTag, mergedTopo[0].payload, sizeof(mergedTag));
  assert(mergedTag.noteId == 10);
  std::memcpy(&mergedTag, mergedTopo[1].payload, sizeof(mergedTag));
  assert(mergedTag.noteId == 11);
  std::memcpy(&mergedTag, mergedTopo[2].payload, sizeof(mergedTag));
  assert(mergedTag.noteId == 20);
  std::memcpy(&mergedTag, mergedTopo[3].payload, sizeof(mergedTag));
  assert(mergedTag.noteId == 21);

  if (daw::patcher_process_audio_passthrough) {
    float ch0[8]{};
    float ch1[8]{};
    float* channels[2]{ch0, ch1};
    daw::PatcherContext audioCtx{};
    audioCtx.abi_version = 1;
    audioCtx.num_frames = 8;
    audioCtx.audio_channels = channels;
    audioCtx.num_channels = 2;
    daw::patcher_process_audio_passthrough(&audioCtx);
    assert(ch0[0] == 1.0f);
    assert(ch1[0] == 1.0f);
  }

  std::cout << "patcher_tests_main: ok" << std::endl;
  return 0;
}
