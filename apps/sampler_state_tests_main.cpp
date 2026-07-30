// THE SAMPLER DOCUMENT: keymap resolution, the derived facts, and the 15-character param-id wall.
//
// The param-id tests exist because the wall is real and was already hit once on paper: the engine
// REFUSES any automation id of 16+ characters (apps/daw_engine_main.cpp:8442), and the first
// namespace written for this device ("modset:1:resonance", 18 chars) would have been rejected on
// every write. The refusal is loud, but it would have shipped as "automation mysteriously does not
// work on resonance" — worse than a compile error and far cheaper to prevent than to diagnose.
// So the budget is asserted here, over the whole id range, and an overrun is a build failure.
#include <cstdint>
#include <cstdio>
#include <string>

#include "apps/sampler_state.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
}

template <typename A, typename B>
void checkEq(const A& got, const B& want, const char* what) {
  if (!(got == static_cast<A>(want))) {
    std::printf("FAIL %s: got %lld want %lld\n", what, static_cast<long long>(got),
                static_cast<long long>(want));
    ++g_fail;
  }
}

daw::SamplerSlot drumSlot(uint16_t id, uint8_t key) {
  daw::SamplerSlot s;
  s.id = id;
  s.keyLow = s.keyHigh = s.rootKey = key;  // Battery-fixed, expressed as a derivation
  return s;
}

daw::SamplerSlot zoneSlot(uint16_t id, uint8_t lo, uint8_t hi, uint8_t root) {
  daw::SamplerSlot s;
  s.id = id;
  s.keyLow = lo;
  s.keyHigh = hi;
  s.rootKey = root;
  return s;
}

}  // namespace

int main() {
  // ---- PARAM IDS FIT, ACROSS THE WHOLE RANGE. This is the survey's finding turned into a build
  // failure. Not a spot check: every field name at the largest id anyone can reach.
  {
    const char* slotFields[] = {"gain", "pan", "tune", "cut", "res", "st", "vg"};
    for (const char* f : slotFields) {
      for (uint16_t id : {uint16_t(1), uint16_t(99), uint16_t(999), uint16_t(65535)}) {
        const std::string s = daw::slotParamId(id, f);
        if (!daw::paramIdFits(s)) {
          std::printf("FAIL slot param id '%s' is %zu chars, budget is %zu\n", s.c_str(),
                      s.size(), daw::kMaxParamIdLen);
          ++g_fail;
        }
      }
    }
    const char* setFields[] = {"cut", "res", "flt"};
    for (const char* f : setFields) {
      for (uint16_t id : {uint16_t(1), uint16_t(999), uint16_t(65535)}) {
        const std::string s = daw::modSetParamId(id, f);
        if (!daw::paramIdFits(s)) {
          std::printf("FAIL modset param id '%s' is %zu chars\n", s.c_str(), s.size());
          ++g_fail;
        }
      }
    }
    for (uint16_t set : {uint16_t(1), uint16_t(999), uint16_t(65535)}) {
      for (uint16_t mod : {uint16_t(1), uint16_t(999), uint16_t(65535)}) {
        for (char w : {'d', 'r'}) {
          const std::string s = daw::modulatorParamId(set, mod, w);
          if (!daw::paramIdFits(s)) {
            std::printf("FAIL modulator param id '%s' is %zu chars\n", s.c_str(), s.size());
            ++g_fail;
          }
        }
      }
    }
    // ...and the guard itself rejects what it should, or it would pass everything.
    check(!daw::paramIdFits("modset:1:resonance"),
          "the ORIGINAL namespace is correctly refused — 18 chars against a 15 budget. Without "
          "this the fits() check could be a tautology");
    check(!daw::paramIdFits(""), "an empty id is not valid");
    check(daw::paramIdFits("m999.999.d"), "the worst realistic case fits with room to spare");
  }

  // ---- THE MAPPING MODE IS DERIVED, NOT STORED. §1: Battery-fixed vs Simpler-zone is
  // keyLow==keyHigh==rootKey vs keyLow<keyHigh, so pitch means exactly one thing either way and
  // there is no mode enum that could disagree with the keys.
  {
    check(drumSlot(1, 36).isFixedPitch(), "a drum slot reads as fixed pitch from its keys alone");
    check(!zoneSlot(2, 36, 72, 60).isFixedPitch(), "a zone slot does not");
    // The trap: same root, wider zone. A naive `rootKey == keyLow` would call this fixed.
    check(!zoneSlot(3, 60, 72, 60).isFixedPitch(),
          "a zone whose root happens to be its low key is still a zone");
  }

  // ---- THE KEYMAP. A kit: eight drums on eight keys, nothing anywhere else.
  {
    daw::SamplerState st;
    for (uint16_t i = 0; i < 8; ++i) {
      st.slots.push_back(drumSlot(static_cast<uint16_t>(i + 1), static_cast<uint8_t>(36 + i)));
    }
    daw::SamplerKeymap km;
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 36, 100, 0), 1, "C-1 resolves to the first slot");
    checkEq(daw::resolveSlot(st, km, 43, 100, 0), 8, "the eighth key resolves to the eighth slot");
    checkEq(daw::resolveSlot(st, km, 60, 100, 0), 0,
            "an UNMAPPED key resolves to nothing, rather than to the nearest slot — a kit with a "
            "hole in it must be silent there, not quietly play the wrong drum");
    checkEq(daw::resolveSlot(st, km, 127, 100, 0), 0, "and the top of the range is safe");
  }

  // ---- VELOCITY LAYERS: the NARROWEST window containing the hit wins. Declaration order must
  // not decide it, or authoring a quiet layer on top of a full-range slot silently does nothing.
  {
    daw::SamplerState st;
    daw::SamplerSlot full = drumSlot(1, 38);  // 0..127, authored first
    daw::SamplerSlot soft = drumSlot(2, 38);
    soft.velLow = 0;
    soft.velHigh = 40;
    daw::SamplerSlot hard = drumSlot(3, 38);
    hard.velLow = 100;
    hard.velHigh = 127;
    st.slots = {full, soft, hard};
    daw::SamplerKeymap km;
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 38, 20, 0), 2, "a soft hit takes the soft layer");
    checkEq(daw::resolveSlot(st, km, 38, 120, 0), 3, "a hard hit takes the hard layer");
    checkEq(daw::resolveSlot(st, km, 38, 70, 0), 1,
            "a hit between the layers takes the full-range slot");
  }

  // ---- A VELOCITY GAP FALLS BACK RATHER THAN GOING SILENT. A split with a hole is an authoring
  // mistake; a MISSING HIT is much harder to diagnose than a slightly wrong one, so the failure
  // mode is deliberately the loud one.
  {
    daw::SamplerState st;
    daw::SamplerSlot soft = drumSlot(1, 40);
    soft.velLow = 0;
    soft.velHigh = 30;
    daw::SamplerSlot hard = drumSlot(2, 40);
    hard.velLow = 100;
    hard.velHigh = 127;
    st.slots = {soft, hard};
    daw::SamplerKeymap km;
    km.rebuild(st);
    check(daw::resolveSlot(st, km, 40, 64, 0) != 0,
          "a hit in the GAP between two velocity layers still makes a sound");
  }

  // ---- ROUND-ROBIN CYCLES, and the counter is the caller's so alternates advance per key rather
  // than globally — two toms round-robining must not steal each other's turn.
  {
    daw::SamplerState st;
    for (uint16_t i = 0; i < 3; ++i) {
      daw::SamplerSlot s = drumSlot(static_cast<uint16_t>(i + 1), 45);
      s.selectMode = 1;  // round-robin
      st.slots.push_back(s);
    }
    daw::SamplerKeymap km;
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 45, 100, 0), 1, "round-robin starts at the first alternate");
    checkEq(daw::resolveSlot(st, km, 45, 100, 1), 2, "then the second");
    checkEq(daw::resolveSlot(st, km, 45, 100, 2), 3, "then the third");
    checkEq(daw::resolveSlot(st, km, 45, 100, 3), 1, "and wraps");
    // DETERMINISM: the same counter always gives the same answer. Round-robin driven by anything
    // stateful inside the resolver would make a bounce differ from the audition.
    checkEq(daw::resolveSlot(st, km, 45, 100, 7), daw::resolveSlot(st, km, 45, 100, 7),
            "resolution is a pure function of its inputs");
  }

  // ---- OVERLAPPING ZONES. A multisample instrument: three zones across the keyboard, each with
  // its own root, so varispeed is relative to the right sample.
  {
    daw::SamplerState st;
    st.slots = {zoneSlot(1, 0, 47, 36), zoneSlot(2, 48, 71, 60), zoneSlot(3, 72, 127, 84)};
    daw::SamplerKeymap km;
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 36, 100, 0), 1, "low zone");
    checkEq(daw::resolveSlot(st, km, 60, 100, 0), 2, "middle zone");
    checkEq(daw::resolveSlot(st, km, 100, 100, 0), 3, "high zone");
    checkEq(daw::resolveSlot(st, km, 47, 100, 0), 1, "the boundary belongs to the lower zone");
    checkEq(daw::resolveSlot(st, km, 48, 100, 0), 2, "and the next key to the upper one");
  }

  // ---- THE KEYMAP IS DERIVED. Rebuilding after an edit must reflect the edit; a stale index is
  // the "two facts that disagree" failure in its most literal form.
  {
    daw::SamplerState st;
    st.slots.push_back(drumSlot(1, 36));
    daw::SamplerKeymap km;
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 38, 100, 0), 0, "nothing on 38 yet");
    st.slots.push_back(drumSlot(2, 38));
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 38, 100, 0), 2, "and the rebuild sees the new slot");
    st.slots.erase(st.slots.begin());
    km.rebuild(st);
    checkEq(daw::resolveSlot(st, km, 36, 100, 0), 0, "and the removal");
  }

  // ---- THE DEFAULT MOD SET MAKES A SOUND. A freshly created device that is silent is a device
  // you have to debug before you can use it.
  {
    const daw::SamplerModSet m = daw::defaultModSet(1);
    const daw::SamplerModulator* amp = m.ampEnvelope();
    check(amp != nullptr, "the default mod set HAS an amp envelope");
    if (amp) {
      check(amp->kind == daw::ModKind::Envelope, "which is an envelope");
      check(amp->target == daw::ModTarget::Volume, "targeting volume");
      check(amp->apply == 1, "and MULTIPLYING — an additive volume envelope is not a thing");
      check(amp->env.points.size() == 4, "it is an ADSR, which is four points (R4)");
      check(amp->env.sustainLoopStart == 2 && amp->env.sustainLoopEnd == 2,
            "with the one-point sustain loop that IS the sustain stage");
      check(amp->env.points[1].time == 0,
            "and an INSTANT attack — the first thing anyone drops on a sampler is a drum, and a "
            "10 ms attack on a kick is a defect you have to go and find");
    }
  }

  // ---- ampEnvelope() PICKS THE ENVELOPE, NOT AN LFO THAT HAPPENS TO TARGET VOLUME. Without this
  // a tremolo LFO authored before the envelope would be mistaken for the amp stage, and every
  // voice would end when the LFO said so.
  {
    daw::SamplerModSet m;
    m.id = 1;
    daw::SamplerModulator lfo;
    lfo.id = 1;
    lfo.kind = daw::ModKind::Lfo;
    lfo.target = daw::ModTarget::Volume;
    daw::SamplerModulator env;
    env.id = 2;
    env.kind = daw::ModKind::Envelope;
    env.target = daw::ModTarget::Volume;
    env.env = daw::makeAdsr(1, 2, 500, 3);
    m.modulators = {lfo, env};  // the LFO FIRST, on purpose
    const daw::SamplerModulator* amp = m.ampEnvelope();
    check(amp != nullptr && amp->id == 2,
          "ampEnvelope() skips a volume-targeted LFO and finds the envelope");
  }
  {
    daw::SamplerModSet m;
    m.id = 1;
    check(m.ampEnvelope() == nullptr, "a mod set with no envelope reports none, rather than a "
                                      "dangling pointer into an empty vector");
  }

  // ---- LOOKUPS RETURN NULL FOR WHAT IS NOT THERE. Every one of these is on a path that will be
  // fed ids read out of a project file.
  {
    daw::SamplerState st;
    st.slots.push_back(drumSlot(7, 36));
    st.modSets.push_back(daw::defaultModSet(1));
    st.sources.push_back({3, "a.wav", 0, 0});
    check(st.findSlot(7) != nullptr, "an existing slot is found");
    check(st.findSlot(8) == nullptr, "a missing slot is null, not the nearest");
    check(st.findModSet(1) != nullptr, "an existing mod set is found");
    check(st.findModSet(2) == nullptr, "a missing mod set is null");
    check(st.findSource(3) != nullptr, "an existing source is found");
    check(st.findSource(4) == nullptr, "a missing source is null");
    check(st.findModSet(1)->find(1) != nullptr, "an existing modulator is found");
    check(st.findModSet(1)->find(9) == nullptr, "a missing modulator is null");
  }

  if (g_fail == 0) {
    std::printf("sampler_state_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
