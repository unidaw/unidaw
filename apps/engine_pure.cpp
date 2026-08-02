// Bodies for apps/engine_pure.h. The WHY for each rule is in the header, beside the declaration a
// reader actually looks at; this file is the mechanics only.
#include "apps/engine_pure.h"

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace daw::engine {

bool documentHasPerDeviceGraphs(const daw::ProjectDocument& doc) {
  for (const auto& track : doc.tracks) {
    for (const auto& device : track.chain.devices) {
      if (!device.patcher.nodes.empty()) {
        return true;
      }
    }
  }
  return false;
}

daw::UiSamplerRejectReason samplerReasonFor(const char* why) {
  using R = daw::UiSamplerRejectReason;
  if (why == nullptr) return R::BadValue;
  if (std::strcmp(why, "no_such_slot") == 0) return R::NoSuchSlot;
  if (std::strcmp(why, "no_such_mod_set") == 0) return R::NoSuchModSet;
  if (std::strcmp(why, "no_such_modulator") == 0) return R::NoSuchModulator;
  if (std::strcmp(why, "no_such_source") == 0) return R::NoSuchSource;
  if (std::strcmp(why, "no_such_slice") == 0) return R::NoSuchSliceSet;
  return R::BadValue;  // unknown_field, and anything added later that nobody mapped
}

std::string errorScopeName(const char* family, uint16_t code) {
  static const std::unordered_map<std::string, std::vector<const char*>> kNames = {
      {"routing", {"", "track_missing", "invalid_kind", "invalid_target"}},
      {"chain", {"", "add_failed", "remove_failed", "move_failed", "update_failed"}},
      {"mod", {"", "track_missing", "link_missing", "invalid_kind", "invalid_device",
               "order_violation", "link_exists"}},
  };
  if (family == nullptr) {
    return "code:" + std::to_string(code);
  }
  auto it = kNames.find(family);
  if (it != kNames.end() && code < it->second.size() && *it->second[code]) {
    return it->second[code];
  }
  return "code:" + std::to_string(code);
}

daw::UndoEntry invertUndoEntry(const daw::UndoEntry& entry) {
  daw::UndoEntry inverse = entry;
  switch (entry.type) {
    case daw::UndoType::AddNote:
      inverse.type = daw::UndoType::RemoveNote;
      break;
    case daw::UndoType::RemoveNote:
      inverse.type = daw::UndoType::AddNote;
      break;
    case daw::UndoType::AddHarmony:
      inverse.type = daw::UndoType::RemoveHarmony;
      break;
    case daw::UndoType::RemoveHarmony:
      inverse.type = daw::UndoType::AddHarmony;
      break;
    case daw::UndoType::UpdateHarmony: {
      inverse.type = daw::UndoType::UpdateHarmony;
      std::swap(inverse.harmonyRoot, inverse.harmonyRoot2);
      std::swap(inverse.harmonyScaleId, inverse.harmonyScaleId2);
      break;
    }
    case daw::UndoType::AddChord:
      inverse.type = daw::UndoType::RemoveChord;
      break;
    case daw::UndoType::RemoveChord:
      inverse.type = daw::UndoType::AddChord;
      break;
  }
  return inverse;
}

std::string pluginStateFileName(uint32_t trackId, uint32_t deviceId) {
  return "t" + std::to_string(trackId) + "_d" + std::to_string(deviceId) + ".bin";
}

std::string pluginParamsFileName(uint32_t trackId, uint32_t deviceId) {
  return "t" + std::to_string(trackId) + "_d" + std::to_string(deviceId) + ".params.json";
}

uint64_t clipContentEnd(const daw::MusicalClip& clip) {
  uint64_t end = 0;
  for (const auto& e : clip.events()) {
    uint64_t dur = 0;
    if (e.type == daw::MusicalEventType::Note) {
      dur = e.payload.note.durationNanoticks;
    } else if (e.type == daw::MusicalEventType::Chord) {
      dur = e.payload.chord.durationNanoticks;
    }
    end = std::max(end, e.nanotickOffset + dur);
  }
  return end;
}

void shiftDiffTick(daw::UiDiffPayload& d, uint64_t placementAt) {
  uint64_t t = (static_cast<uint64_t>(d.noteNanotickHi) << 32) | d.noteNanotickLo;
  t += placementAt;
  d.noteNanotickLo = static_cast<uint32_t>(t & 0xffffffffu);
  d.noteNanotickHi = static_cast<uint32_t>((t >> 32) & 0xffffffffu);
}

daw::SamplerModulator* findOrMintEnvelope(daw::SamplerModSet& ms, daw::ModTarget target) {
  for (auto& m : ms.modulators) {
    if (m.kind == daw::ModKind::Envelope && m.target == target) {
      return &m;
    }
  }
  daw::SamplerModulator fresh;
  fresh.id = ms.nextModulatorId++;
  fresh.kind = daw::ModKind::Envelope;
  fresh.target = target;
  fresh.apply = target == daw::ModTarget::Volume ? 1 : 0;
  fresh.depthMilli = 1000;
  ms.modulators.push_back(fresh);
  return &ms.modulators.back();
}

void ensureDefaultModSet(daw::SamplerState& sampler, uint32_t requestedId) {
  if (requestedId != 0 || !sampler.modSets.empty()) {
    return;
  }
  sampler.modSets.push_back(daw::defaultModSet(1));
  sampler.nextModSetId = 2;
}

uint8_t clampMidi(int pitch) {
  if (pitch < 0) {
    return 0;
  }
  if (pitch > 127) {
    return 127;
  }
  return static_cast<uint8_t>(pitch);
}

std::string sampleDisplayName(const std::string& path) {
  std::string stem = std::filesystem::path(path).stem().string();
  if (stem.size() >= daw::kUiSamplerSlotNameBytes) {
    stem.resize(daw::kUiSamplerSlotNameBytes - 1);
  }
  return stem;
}

}  // namespace daw::engine
