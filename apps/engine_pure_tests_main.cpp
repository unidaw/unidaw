// Tests for apps/engine_pure.h — the eleven rules that were lambdas inside main().
//
// NONE of these had a test before, because none of them had a name another translation unit could
// say. Reaching `clampMidi` meant booting an engine, mapping shared memory and driving a chord
// through a command ring; reaching the 32-bit carry in `shiftDiffTick` meant authoring a project
// long enough to push a note past tick 2^32. That is why the edge cases below are the ones that
// were never covered: not because anyone judged them unimportant, but because the cost of asking
// was a whole engine.
//
// So these tests are deliberately weighted toward BOUNDARIES and toward the cases where a
// plausible wrong implementation still passes a happy-path test:
//   - documentHasPerDeviceGraphs must look past the first track and the first device
//   - errorScopeName's code 0 is a HOLE in every family's table, not a name
//   - clipContentEnd must take a max, not the last event
//   - shiftDiffTick must carry across the 32-bit split
//   - findOrMintEnvelope must not match an LFO that shares the target
//   - ensureDefaultModSet must NOT mint for a named-but-absent id
#include "apps/engine_pure.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace daw;
using namespace daw::engine;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

// ---------------------------------------------------------------- documentHasPerDeviceGraphs
void testDocumentHasPerDeviceGraphs() {
  ProjectDocument empty;
  CHECK(!documentHasPerDeviceGraphs(empty));

  // A document full of devices that carry NO graph is the common case — the legacy shape where
  // one global pool holds the authored patcher. Reporting true here would make every save park
  // the pool on a device and lose the distinction this function exists to draw.
  ProjectDocument noGraphs;
  noGraphs.tracks.resize(2);
  noGraphs.tracks[0].chain.devices.resize(2);
  noGraphs.tracks[1].chain.devices.resize(2);
  CHECK(!documentHasPerDeviceGraphs(noGraphs));

  // THE CASE THAT CATCHES A `return` IN THE WRONG LOOP: the graph is on the LAST device of the
  // LAST track. An implementation that returned from the first track, or checked only devices[0],
  // passes the happy path and fails here.
  ProjectDocument deep = noGraphs;
  deep.tracks[1].chain.devices[1].patcher.nodes.resize(1);
  CHECK(documentHasPerDeviceGraphs(deep));

  ProjectDocument first = noGraphs;
  first.tracks[0].chain.devices[0].patcher.nodes.resize(1);
  CHECK(documentHasPerDeviceGraphs(first));

  // A track with no devices at all must not crash or count.
  ProjectDocument bare;
  bare.tracks.resize(3);
  CHECK(!documentHasPerDeviceGraphs(bare));
}

// ---------------------------------------------------------------------------- samplerReasonFor
void testSamplerReasonFor() {
  using R = UiSamplerRejectReason;
  CHECK(samplerReasonFor("no_such_slot") == R::NoSuchSlot);
  CHECK(samplerReasonFor("no_such_mod_set") == R::NoSuchModSet);
  CHECK(samplerReasonFor("no_such_modulator") == R::NoSuchModulator);
  CHECK(samplerReasonFor("no_such_source") == R::NoSuchSource);
  CHECK(samplerReasonFor("no_such_slice") == R::NoSuchSliceSet);

  // The fallback is the whole point of deriving this from the log string: a `why` nobody mapped
  // must still produce a legal wire code rather than reading off the end of a table.
  CHECK(samplerReasonFor("unknown_field") == R::BadValue);
  CHECK(samplerReasonFor("") == R::BadValue);
  CHECK(samplerReasonFor("no_such_slot_extra") == R::BadValue);  // prefix must not match
  CHECK(samplerReasonFor(nullptr) == R::BadValue);               // and a null why must not crash
}

// ------------------------------------------------------------------------------ errorScopeName
void testErrorScopeName() {
  CHECK(errorScopeName("routing", 1) == "track_missing");
  CHECK(errorScopeName("routing", 3) == "invalid_target");
  CHECK(errorScopeName("chain", 4) == "update_failed");
  CHECK(errorScopeName("mod", 6) == "link_exists");

  // CODE 0 IS A HOLE, not a name: every family's table starts with "" so that a zero code — which
  // means "no error" — never formats as the first real reason. The `*it->second[code]` guard is
  // what makes that true, and this is the assertion that keeps it.
  CHECK(errorScopeName("routing", 0) == "code:0");
  CHECK(errorScopeName("chain", 0) == "code:0");

  // Past the end of a family's table, and a family nobody registered.
  CHECK(errorScopeName("routing", 4) == "code:4");
  CHECK(errorScopeName("mod", 99) == "code:99");
  CHECK(errorScopeName("nosuchfamily", 2) == "code:2");
  CHECK(errorScopeName(nullptr, 7) == "code:7");
}

// ----------------------------------------------------------------------------- invertUndoEntry
void testInvertUndoEntry() {
  UndoEntry e;
  e.trackId = 9;
  e.nanotick = 123456;
  e.pitch = 64;
  e.velocity = 100;

  e.type = UndoType::AddNote;
  CHECK(invertUndoEntry(e).type == UndoType::RemoveNote);
  e.type = UndoType::RemoveNote;
  CHECK(invertUndoEntry(e).type == UndoType::AddNote);
  e.type = UndoType::AddHarmony;
  CHECK(invertUndoEntry(e).type == UndoType::RemoveHarmony);
  e.type = UndoType::RemoveHarmony;
  CHECK(invertUndoEntry(e).type == UndoType::AddHarmony);
  e.type = UndoType::AddChord;
  CHECK(invertUndoEntry(e).type == UndoType::RemoveChord);
  e.type = UndoType::RemoveChord;
  CHECK(invertUndoEntry(e).type == UndoType::AddChord);

  // EVERY OTHER FIELD RIDES ALONG. An inverse that reset the payload would undo the right KIND of
  // edit at the wrong place, which is far worse than not undoing at all.
  e.type = UndoType::AddNote;
  const UndoEntry inv = invertUndoEntry(e);
  CHECK(inv.trackId == 9);
  CHECK(inv.nanotick == 123456);
  CHECK(inv.pitch == 64);
  CHECK(inv.velocity == 100);

  // UpdateHarmony is the only self-inverse type, and it carries BEFORE and AFTER in two pairs.
  // Inverting swaps them; inverting twice must return the original, which is the property that
  // makes undo/redo round-trip rather than drift.
  UndoEntry h;
  h.type = UndoType::UpdateHarmony;
  h.harmonyRoot = 3;
  h.harmonyScaleId = 11;
  h.harmonyRoot2 = 7;
  h.harmonyScaleId2 = 22;
  const UndoEntry once = invertUndoEntry(h);
  CHECK(once.type == UndoType::UpdateHarmony);
  CHECK(once.harmonyRoot == 7);
  CHECK(once.harmonyScaleId == 22);
  CHECK(once.harmonyRoot2 == 3);
  CHECK(once.harmonyScaleId2 == 11);
  const UndoEntry twice = invertUndoEntry(once);
  CHECK(twice.harmonyRoot == 3);
  CHECK(twice.harmonyScaleId == 11);
  CHECK(twice.harmonyRoot2 == 7);
  CHECK(twice.harmonyScaleId2 == 22);
}

// ------------------------------------------------------------------------------ plugin naming
void testPluginFileNames() {
  CHECK(pluginStateFileName(0, 0) == "t0_d0.bin");
  CHECK(pluginStateFileName(3, 12) == "t3_d12.bin");
  CHECK(pluginParamsFileName(3, 12) == "t3_d12.params.json");

  // The two must never collide, and the id order must not be commutative — (3,12) and (12,3) are
  // different devices and a name that conflated them would restore one plugin's state into
  // another's.
  CHECK(pluginStateFileName(3, 12) != pluginStateFileName(12, 3));
  CHECK(pluginStateFileName(3, 12) != pluginParamsFileName(3, 12));
}

// ------------------------------------------------------------------------------ clipContentEnd
MusicalEvent note(uint64_t tick, uint64_t duration, uint8_t pitch = 60) {
  MusicalEvent e;
  e.nanotickOffset = tick;
  e.type = MusicalEventType::Note;
  e.payload.note.pitch = pitch;
  e.payload.note.velocity = 100;
  e.payload.note.column = 0;
  e.payload.note.durationNanoticks = duration;
  e.payload.note.noteId = 0;
  return e;
}

void testClipContentEnd() {
  MusicalClip empty;
  CHECK(clipContentEnd(empty) == 0);

  MusicalClip one;
  one.addEvent(note(1000, 500));
  CHECK(clipContentEnd(one) == 1500);

  // THE MAX, NOT THE LAST. Events sort by offset, so a long note early and a short note late
  // means the furthest content end is NOT the last event's. An implementation that returned the
  // last event's extent passes every clip whose longest note happens to be last.
  MusicalClip overlap;
  overlap.addEvent(note(0, 10000));  // sorts FIRST (offset 0), ends at 10000
  overlap.addEvent(note(500, 100));  // sorts LAST (offset 500), ends at 600
  CHECK(clipContentEnd(overlap) == 10000);

  MusicalClip chord;
  MusicalEvent c;
  c.nanotickOffset = 2000;
  c.type = MusicalEventType::Chord;
  c.payload.chord.durationNanoticks = 3000;
  chord.addEvent(c);
  CHECK(clipContentEnd(chord) == 5000);

  // A zero-duration note still extends the clip to its own offset — a tracker OFF or a note whose
  // length is "until the next one" must not read as "the clip ends before this event".
  MusicalClip zero;
  zero.addEvent(note(7777, 0));
  CHECK(clipContentEnd(zero) == 7777);
}

// ------------------------------------------------------------------------------- shiftDiffTick
void testShiftDiffTick() {
  UiDiffPayload d{};
  d.noteNanotickLo = 1000;
  d.noteNanotickHi = 0;
  shiftDiffTick(d, 500);
  CHECK(d.noteNanotickLo == 1500);
  CHECK(d.noteNanotickHi == 0);

  // A zero shift must be exactly identity: a clip placed at tick 0 is the common case and must not
  // acquire an off-by-one from being "shifted".
  UiDiffPayload id{};
  id.noteNanotickLo = 42;
  id.noteNanotickHi = 7;
  shiftDiffTick(id, 0);
  CHECK(id.noteNanotickLo == 42);
  CHECK(id.noteNanotickHi == 7);

  // THE CARRY. The tick is split across two 32-bit wire fields, and this is the only place the
  // split is reassembled. Adding to a lo near the boundary must carry into hi — an implementation
  // that added to lo alone silently wraps a note back near tick 0, roughly 74 hours into a project
  // at 960000 nanoticks per quarter, which is exactly the kind of bug nobody authors a fixture for.
  UiDiffPayload carry{};
  carry.noteNanotickLo = 0xffffffffu;
  carry.noteNanotickHi = 0;
  shiftDiffTick(carry, 1);
  CHECK(carry.noteNanotickLo == 0);
  CHECK(carry.noteNanotickHi == 1);

  // And a shift whose own value exceeds 32 bits.
  UiDiffPayload big{};
  big.noteNanotickLo = 5;
  big.noteNanotickHi = 0;
  shiftDiffTick(big, (uint64_t{3} << 32) + 7);
  CHECK(big.noteNanotickLo == 12);
  CHECK(big.noteNanotickHi == 3);
}

// -------------------------------------------------------------------------- findOrMintEnvelope
void testFindOrMintEnvelope() {
  SamplerModSet ms;
  ms.nextModulatorId = 5;
  CHECK(ms.modulators.empty());

  SamplerModulator* vol = findOrMintEnvelope(ms, ModTarget::Volume);
  CHECK(vol != nullptr);
  CHECK(ms.modulators.size() == 1);
  CHECK(vol->kind == ModKind::Envelope);
  CHECK(vol->target == ModTarget::Volume);
  CHECK(vol->id == 5);
  CHECK(ms.nextModulatorId == 6);
  // VOLUME MULTIPLIES. An amp envelope that added would never reach silence however deep it went.
  CHECK(vol->apply == 1);
  CHECK(vol->depthMilli == 1000);
  // COPIED OUT BEFORE THE NEXT MINT, deliberately. The returned pointer is into
  // `ms.modulators`, and the mint below push_backs — which can reallocate and dangle `vol`.
  // The first draft of this test compared `again->id == vol->id` and read freed memory; it
  // failed, which is how the contract got written down in engine_pure.h. Both engine call sites
  // consume the pointer immediately and are unaffected.
  const uint16_t volId = vol->id;

  // Everything else ADDS to a base value.
  SamplerModulator* cut = findOrMintEnvelope(ms, ModTarget::Cutoff);
  CHECK(cut != nullptr);
  CHECK(cut->apply == 0);
  CHECK(ms.modulators.size() == 2);
  CHECK(cut->id == 6);

  // FINDING, not minting: asking again for Volume must return the existing one and add nothing.
  const size_t before = ms.modulators.size();
  SamplerModulator* again = findOrMintEnvelope(ms, ModTarget::Volume);
  CHECK(ms.modulators.size() == before);
  CHECK(again->id == volId);

  // AN LFO ON THE SAME TARGET IS NOT AN ENVELOPE. A match on target alone would hand back the LFO
  // and let an envelope edit rewrite an LFO's fields — the kind is half the identity.
  SamplerModSet withLfo;
  SamplerModulator lfo;
  lfo.id = 1;
  lfo.kind = ModKind::Lfo;
  lfo.target = ModTarget::Pitch;
  withLfo.modulators.push_back(lfo);
  withLfo.nextModulatorId = 2;
  SamplerModulator* env = findOrMintEnvelope(withLfo, ModTarget::Pitch);
  CHECK(withLfo.modulators.size() == 2);
  CHECK(env->kind == ModKind::Envelope);
  CHECK(env->id == 2);
}

// -------------------------------------------------------------------------- ensureDefaultModSet
void testEnsureDefaultModSet() {
  // Empty + "the default" (0) → mint, so an envelope command has somewhere to land.
  SamplerState fresh;
  CHECK(fresh.modSets.empty());
  ensureDefaultModSet(fresh, 0);
  CHECK(fresh.modSets.size() == 1);
  CHECK(fresh.modSets[0].id == 1);
  CHECK(fresh.nextModSetId == 2);

  // Already has one + 0 → leave it entirely alone. Minting again would give the sampler two
  // default mod sets and the second would shadow every slot pointing at the first.
  ensureDefaultModSet(fresh, 0);
  CHECK(fresh.modSets.size() == 1);

  // A NAMED id that does not exist must NOT mint. This is the property the whole design rests on:
  // `--mod-set 7` when there is no 7 is a caller naming something absent, and substituting a
  // different mod set would silently edit the wrong thing. It stays empty and the caller's command
  // is refused downstream with no_such_mod_set.
  SamplerState named;
  ensureDefaultModSet(named, 7);
  CHECK(named.modSets.empty());
  ensureDefaultModSet(named, 1);
  CHECK(named.modSets.empty());
}

// -------------------------------------------------------------------------------- clampMidi
void testClampMidi() {
  CHECK(clampMidi(0) == 0);
  CHECK(clampMidi(60) == 60);
  CHECK(clampMidi(127) == 127);
  // Both boundaries are INCLUSIVE on the inside and saturating on the outside.
  CHECK(clampMidi(-1) == 0);
  CHECK(clampMidi(128) == 127);
  CHECK(clampMidi(-1000000) == 0);
  CHECK(clampMidi(1000000) == 127);
}

}  // namespace

int main() {
  testDocumentHasPerDeviceGraphs();
  testSamplerReasonFor();
  testErrorScopeName();
  testInvertUndoEntry();
  testPluginFileNames();
  testClipContentEnd();
  testShiftDiffTick();
  testFindOrMintEnvelope();
  testEnsureDefaultModSet();
  testClampMidi();

  if (g_fail == 0) {
    std::printf("engine_pure_tests: PASS\n");
    return 0;
  }
  std::printf("engine_pure_tests: FAIL (%d)\n", g_fail);
  return 1;
}
