#pragma once

// THE SAMPLER'S DOCUMENT. Owned by the command thread, serialized under "sampler" in the device
// object, never read by the audio thread — what the audio thread reads is the flattened snapshot
// built from this (docs/SAMPLER_DESIGN.md §3.5).
//
// The design rulings this file implements, so the reasons are next to the fields rather than only
// in the doc:
//
//   R1  no per-slot device chains, ever. `outputStem` routes a slot to an aux bus and
//       reconcileChildTracks turns that into a REAL track. A slot is not a rack.
//   R2  which sound a note plays is a per-NOTE field, so slots need stable ids and nothing else.
//   R4  envelopes are multipoint and loopable; ADSR is a view of the same points. The shape and
//       its clock live in apps/sampler_envelope.h — this file only references them.
//
// DERIVED, NOT STORED, throughout: slice extents come from marker order, the mapping "mode" comes
// from keyLow/keyHigh/rootKey, and the keymap index is rebuilt rather than saved. Two facts about
// one thing must never disagree.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "apps/patcher_abi.h"
#include "apps/sampler_envelope.h"

namespace daw {

// A source the device references. `localId` is STABLE IN THE PROJECT; the WaveformStore sourceId
// is runtime-only and re-derived at load — persisting one would be the hostSlotIndex bug in a new
// domain, and that bug cost this project a Zebra2 that reloaded as an Analog Heat.
struct SamplerSource {
  uint16_t localId = 0;
  std::string path;         // project-relative, resolved at load
  uint64_t contentKey = 0;  // recomputed at load; a mismatch is reported, never silently accepted
  uint32_t sourceId = 0;    // RUNTIME ONLY
};

// A slice marker. Extent is DERIVED from marker order; identity is STORED. Inserting a marker
// mints a new id and shortens its predecessor — it never renumbers, which is what lets a chop be
// re-cut while it plays without every note downstream silently pointing somewhere else.
struct SliceMarker {
  uint16_t id = 0;
  uint64_t frame = 0;  // in level-0 source frames
  int16_t tuneCents = 0;
  uint8_t reverse = 0;
  uint16_t modSetId = 0;  // 0 = inherit the slot's
};

struct SliceSet {
  uint16_t sourceLocalId = 0;
  uint16_t nextMarkerId = 1;
  std::vector<SliceMarker> markers;  // sorted by frame; ids never reordered
};

enum class ModTarget : uint8_t { Volume = 0, Panning = 1, Pitch = 2, Cutoff = 3, Resonance = 4 };
enum class ModKind : uint8_t { Envelope = 0, Lfo = 1 };

// One modulator. A LIST of these rather than a fixed set of per-domain fields, so "two LFOs on
// cutoff" needs no new struct and a domain nobody uses costs nothing.
struct SamplerModulator {
  uint16_t id = 0;  // stable; what an automation lane names
  ModTarget target = ModTarget::Volume;
  ModKind kind = ModKind::Envelope;
  int16_t depthMilli = 1000;  // signed. AUTOMATABLE.
  uint8_t apply = 0;          // 0 add, 1 multiply (multiply is right for Volume)
  uint16_t rateMilli = 1000;  // time-scale multiplier, 250..4000. AUTOMATABLE.
  // 0 = microseconds (a decay that means the same at any tempo — right for drums)
  // 1 = nanoticks   (an envelope that follows the project — right for a bar-long sweep)
  // ONE field decides it for the whole envelope. A "sync" flag layered over an absolute time
  // would be two facts about one duration.
  uint8_t timeBase = 0;
  // PURE UI HINT: 0 = the ADSR sliders, 1 = the pencil. The runner ignores it entirely.
  //
  // It exists so the editor is never INFERRED from the shape. Sniffing "four points with
  // sustainLoop{2,2}?" would flip the editor out from under someone the moment they hand-drew a
  // four-point curve — the same failure §1 already rules against for the kit/sample view toggle.
  // The ADSR editor sets 0; the first pencil stroke sets 1, and it never flips back on its own.
  uint8_t editor = 0;
  EnvShape env{};           // ModKind::Envelope
  PatcherLfoConfig lfo{};   // ModKind::Lfo — reuses the patcher's LFO unchanged
};

// The Renoise steal: settings shared BY REFERENCE, so sixteen slots point at two envelopes and
// "shorten the kit's decay" is one edit rather than sixteen. This is the single fix for Battery's
// loudest annoyance and it costs one uint16 per slot.
struct SamplerModSet {
  uint16_t id = 0;
  std::string name;
  uint8_t filterType = 0;  // 0 off, 1 LP12, 2 LP24, 3 HP, 4 BP
  uint16_t cutoffMilli = 1000, resonanceMilli = 0;
  uint16_t nextModulatorId = 1;
  std::vector<SamplerModulator> modulators;

  const SamplerModulator* find(uint16_t id) const {
    for (const auto& m : modulators) {
      if (m.id == id) {
        return &m;
      }
    }
    return nullptr;
  }
  // The amp envelope: the first Volume-targeted envelope. There is deliberately no "which one is
  // the amp envelope" field — a second fact about the same thing is a second thing to disagree.
  const SamplerModulator* ampEnvelope() const {
    for (const auto& m : modulators) {
      if (m.kind == ModKind::Envelope && m.target == ModTarget::Volume) {
        return &m;
      }
    }
    return nullptr;
  }
};

// NNA — what happens to the PREVIOUS voice in the same (column, slot) when a new note arrives.
// IT's answer, and the tracker-native one: it is a voice rule, not a 16-group bookkeeping table.
enum class SamplerNna : uint8_t { Cut = 0, NoteOff = 1, Continue = 2 };

struct SamplerSlot {
  uint16_t id = 0;  // STABLE. This is what a row's `sound` field names.
  std::string name;
  uint16_t sourceLocalId = 0;
  uint16_t sliceId = 0;  // 0 = the whole source
  uint64_t startFrame = 0, endFrame = 0;
  uint64_t loopStartFrame = 0, loopEndFrame = 0, loopXfadeFrames = 0;
  uint8_t loopMode = 0;     // 0 off, 1 forward, 2 ping-pong, 3 backward  (S3)
  uint8_t sustainLoop = 0;  // 1 = the loop releases at note-off and plays out
  // THERE IS NO MAPPING-MODE ENUM. Battery-fixed is keyLow == keyHigh == rootKey; Simpler-zone is
  // keyLow < keyHigh. Derived, not stored, so pitch means exactly one thing either way.
  uint8_t keyLow = 0, keyHigh = 127, rootKey = 60;
  int16_t pitchTrackMilli = 1000;  // 1000 = full varispeed, 0 = fixed pitch (a drum)
  int16_t tuneCents = 0;
  uint8_t velLow = 0, velHigh = 127;
  uint16_t layerGroup = 0;  // slots sharing (zone, layerGroup) are alternates
  uint8_t selectMode = 0;   // 0 velocity, 1 round-robin, 2 random, 3 cycle-per-row
  uint8_t gate = 0;         // 0 = one-shot (ignores note-off), 1 = gated
  uint8_t reverse = 0;
  int16_t gainMillibels = 0;
  int16_t panThousandths = 0;
  uint8_t voiceGroup = 0;  // 0 = none; equal non-zero groups cut each other (open/closed hat)
  SamplerNna nna = SamplerNna::Cut;
  uint8_t polyphony = 0;        // 0 = inherit the device
  uint32_t chokeFadeUs = 3000;  // 3 ms, so a choke is not a click
  uint16_t modSetId = 1;
  uint8_t outputStem = 0;  // 0 = main; 1..15 = an aux stereo stem -> a child track (R1)
  uint8_t quality = 1;     // 0 Vintage, 1 Fast, 2 Studio — a SOUND, not a setting (S3)

  bool isFixedPitch() const { return keyLow == keyHigh && keyLow == rootKey; }
};

struct SamplerState {
  uint16_t nextSlotId = 1, nextSourceId = 1, nextModSetId = 1;
  uint8_t stemCount = 0;  // set at instantiation; changing it renegotiates buses
  uint8_t voiceCap = 64;
  uint8_t defaultView = 0;  // 0 kit, 1 sample — seeded at load by drop arity, then user-owned
  // WHAT `gate` A NEWLY MINTED SLOT GETS. 0 = one-shot (ignores note-off), 1 = gated.
  //
  // It SEEDS and then stops mattering: sampler-load and sampler-slice stamp it onto slots they
  // create, and from that moment the slot's own `gate` is the authority. NOT a live override — a
  // device flag that overrode slots at playback would be two facts about one thing for the voice
  // to arbitrate on every note, which is the shape that had the kit read-back disagreeing with
  // the model and the patcher pool disagreeing with the device graphs. A seed cannot drift
  // because nothing consults it once the slot exists. `defaultView` above works the same way.
  //
  // WHY IT EXISTS: `gate` defaulted to 0 because neither load nor slice ever set it, so every
  // slot they made inherited "one-shot" from this struct's initialiser rather than from anyone's
  // decision. Right for drums by accident. A chopped break is 64 slices and setting the field 64
  // times is not a workflow; a bank-level default is (owner, 2026-07-31: "could that be a setting
  // per bank? 'ignore note-offs'").
  uint8_t defaultGate = 0;
  std::vector<SamplerSource> sources;
  std::vector<SliceSet> sliceSets;
  std::vector<SamplerModSet> modSets;
  std::vector<SamplerSlot> slots;  // DISPLAY order, not id order

  const SamplerSource* findSource(uint16_t localId) const {
    for (const auto& s : sources) {
      if (s.localId == localId) {
        return &s;
      }
    }
    return nullptr;
  }
  const SamplerSlot* findSlot(uint16_t id) const {
    for (const auto& s : slots) {
      if (s.id == id) {
        return &s;
      }
    }
    return nullptr;
  }
  // THE LOWEST SLOT ID. What a blank `sound` plays on a sound-addressed-only track, where
  // pitch may not select one (owner ruling, docs/SAMPLER_DESIGN.md section 8 Q2).
  //
  // Lowest id rather than first-in-vector, because the vector order is an editing artifact and
  // this has to be the same slot after a reorder. Deterministic and derived, so there is no
  // per-track "selected slot" to keep in sync — a second fact about the same thing is how the
  // read-back and the model came to disagree before.
  uint16_t lowestSlotId() const {
    uint16_t best = 0;
    for (const auto& s : slots) {
      if (s.id != 0 && (best == 0 || s.id < best)) {
        best = s.id;
      }
    }
    return best;
  }
  const SamplerModSet* findModSet(uint16_t id) const {
    for (const auto& m : modSets) {
      if (m.id == id) {
        return &m;
      }
    }
    return nullptr;
  }
};

// A default mod set, so a freshly created device makes a sound rather than silence. The amp
// envelope is an ADSR — which is four points and a one-point sustain loop, not a different kind
// of envelope (R4) — with an instant attack, because the first thing anyone drops on a sampler is
// a drum and a 10 ms attack on a kick is a defect you have to go and find.
inline SamplerModSet defaultModSet(uint16_t id) {
  SamplerModSet m;
  m.id = id;
  m.name = "default";
  SamplerModulator amp;
  amp.id = 1;
  amp.target = ModTarget::Volume;
  amp.kind = ModKind::Envelope;
  amp.apply = 1;  // multiply — the only sane law for a volume envelope
  amp.timeBase = 0;
  amp.env = makeAdsr(0, 0, 1000, 5000);  // instant on, full sustain, 5 ms release
  m.nextModulatorId = 2;
  m.modulators.push_back(amp);
  return m;
}

// ---------------------------------------------------------------------------------------------
// KEYMAP RESOLUTION — the one rule stated once so it can never drift.
//
//   sound != 0   the slot IS that id, and `pitch` means varispeed relative to its rootKey.
//   sound == 0   the slot is found by (pitch, velocity) through the keymap, and `pitch` means
//                exactly the same thing.
//
// Pitch has one meaning either way. That is what makes "the same snare at five pitches" a row
// edit rather than a device edit.

// O(1) lookup rebuilt on the command thread at edit time and swapped in by pointer — never
// searched linearly on the audio thread, and never saved, because it is derived.
struct SamplerKeymap {
  // Slot indices per key, in slot order. A key with several slots has velocity layers or
  // round-robin alternates; the selector picks among them.
  std::array<std::vector<uint16_t>, 128> byKey{};

  void rebuild(const SamplerState& st) {
    for (auto& v : byKey) {
      v.clear();
    }
    for (const auto& slot : st.slots) {
      const uint8_t lo = std::min(slot.keyLow, slot.keyHigh);
      const uint8_t hi = std::max(slot.keyLow, slot.keyHigh);
      for (uint32_t k = lo; k <= hi && k < 128; ++k) {
        byKey[k].push_back(slot.id);
      }
    }
  }
};

// Resolves (pitch, velocity) to a slot id, or 0 if nothing is mapped there. `rrCounter` advances
// for round-robin; pass a per-(device, key) counter so alternates cycle independently per key.
inline uint16_t resolveSlot(const SamplerState& st,
                            const SamplerKeymap& km,
                            uint8_t pitch,
                            uint8_t velocity,
                            uint32_t rrCounter) {
  if (pitch >= 128) {
    return 0;
  }
  const auto& candidates = km.byKey[pitch];
  if (candidates.empty()) {
    return 0;
  }
  // Velocity layers first: a slot whose velocity window excludes this hit is not a candidate at
  // all, whatever the select mode says.
  uint16_t inWindow[16];
  uint32_t n = 0;
  for (uint16_t id : candidates) {
    const SamplerSlot* s = st.findSlot(id);
    if (!s) {
      continue;
    }
    if (velocity < s->velLow || velocity > s->velHigh) {
      continue;
    }
    if (n < 16) {
      inWindow[n++] = id;
    }
  }
  if (n == 0) {
    // Nothing matched the velocity window. Fall back to the first slot on the key rather than
    // going silent: a velocity split with a gap in it is an authoring mistake, and a missing hit
    // is much harder to diagnose than a slightly wrong one.
    return candidates.front();
  }
  if (n == 1) {
    return inWindow[0];
  }
  const SamplerSlot* first = st.findSlot(inWindow[0]);
  const uint8_t mode = first ? first->selectMode : 0;
  if (mode == 1 || mode == 3) {  // round-robin / cycle-per-row
    return inWindow[rrCounter % n];
  }
  if (mode == 2) {  // random — seeded by the caller, never rand()
    return inWindow[rrCounter % n];
  }
  // Velocity: the narrowest window that contains the hit, so an overlapping "layer" authored on
  // top of a full-range slot wins rather than losing to declaration order.
  uint16_t best = inWindow[0];
  uint32_t bestSpan = 256;
  for (uint32_t i = 0; i < n; ++i) {
    const SamplerSlot* s = st.findSlot(inWindow[i]);
    if (!s) {
      continue;
    }
    const uint32_t span = static_cast<uint32_t>(s->velHigh) - s->velLow;
    if (span < bestSpan) {
      bestSpan = span;
      best = inWindow[i];
    }
  }
  return best;
}

// ---------------------------------------------------------------------------------------------
// PARAM IDS — and the 15-character wall they have to fit through.
//
// UiAutomationPointPayload.paramId is char[16] and the engine REFUSES any id of 16 or more
// characters (apps/daw_engine_main.cpp:8442), because the read-back slot nul-terminates inside
// its own 16 bytes: a 16-byte id would be written in full and read back one short, so the write
// and the answer would name different lanes forever. The real budget is 15.
//
// The obvious spelling blows it — "modset:1:resonance" is 18 — so the namespace is a dotted path
// with one-letter roots. Worst case at three-digit ids is "m999.999.d" at 10 characters.
inline constexpr size_t kMaxParamIdLen = 15;

inline std::string slotParamId(uint16_t slotId, const char* field) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "s%u.%s", static_cast<unsigned>(slotId), field);
  return buf;
}
inline std::string modSetParamId(uint16_t setId, const char* field) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "m%u.%s", static_cast<unsigned>(setId), field);
  return buf;
}
inline std::string modulatorParamId(uint16_t setId, uint16_t modId, char which) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "m%u.%u.%c", static_cast<unsigned>(setId),
                static_cast<unsigned>(modId), which);
  return buf;
}

// Every generated id must fit, and the check lives with the generator rather than in a doc — a
// budget nobody can execute is a budget that gets exceeded. Called by the tests over the full id
// range, so an overrun is a build failure and not a mysterious "automation does not work on
// resonance" months later.
inline bool paramIdFits(const std::string& id) {
  return !id.empty() && id.size() <= kMaxParamIdLen;
}

}  // namespace daw
