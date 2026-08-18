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
#include "apps/device_id_migration.h"
#include "apps/engine_command_mutates.h"
#include "apps/host_chain_buffers.h"

#include <cstdint>
#include <cstdio>
#include <vector>
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
  // Addressing the wrong DEVICE, which used to be indistinguishable from a missing slot.
  CHECK(samplerReasonFor("no_such_device") == R::NoSuchDevice);
  CHECK(samplerReasonFor("not_a_sampler") == R::NotASampler);

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
  // The two loose-integer helpers that used to be tested here are gone; the name has ONE
  // definition now (daw::artifactLeafName) and one legacy spelling that requires a key.
  CHECK(daw::artifactLeafName(0, 0, daw::ArtifactKind::StateBlob) == "t0_d0.bin");
  CHECK(daw::artifactLeafName(3, 12, daw::ArtifactKind::StateBlob) == "t3_d12.bin");
  CHECK(daw::artifactLeafName(3, 12, daw::ArtifactKind::ParameterManifest) ==
        "t3_d12.params.json");

  // The two must never collide, and the id order must not be commutative — (3,12) and (12,3) are
  // different devices and a name that conflated them would restore one plugin's state into
  // another's.
  CHECK(daw::artifactLeafName(3, 12, daw::ArtifactKind::StateBlob) !=
        daw::artifactLeafName(12, 3, daw::ArtifactKind::StateBlob));
  CHECK(daw::artifactLeafName(3, 12, daw::ArtifactKind::StateBlob) !=
        daw::artifactLeafName(3, 12, daw::ArtifactKind::ParameterManifest));

  // THE LEGACY SPELLING AGREES WITH THE CANONICAL ONE. A schema 1-5 file and a schema-6 file for
  // the same {track, device} have the same NAME; only the directory differs. If these two ever
  // disagreed, a migrated project would look for its blob under a name nothing ever wrote.
  const daw::LegacyArtifactKey key{3, 12};
  CHECK(daw::legacyArtifactLeafName(key, daw::ArtifactKind::StateBlob) ==
        daw::artifactLeafName(3, 12, daw::ArtifactKind::StateBlob));
  CHECK(daw::legacyArtifactLeafName(key, daw::ArtifactKind::ParameterManifest) ==
        daw::artifactLeafName(3, 12, daw::ArtifactKind::ParameterManifest));
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


// ---------------------------------------------------------- the ruler's rule, in three meters
constexpr uint64_t kQ = daw::kNanoticksPerQuarter;
constexpr uint64_t kBar44 = 4 * kQ;

void testBarTicksIn44() {
  daw::TimeSignatureMap meter;  // default-constructed: one 4/4 point at 0

  CHECK(barStartTick(&meter, 0) == 0);
  CHECK(barEndTick(&meter, 0) == kBar44);
  CHECK(barStartTick(&meter, kBar44 - 1) == 0);
  CHECK(barEndTick(&meter, kBar44 - 1) == kBar44);
  CHECK(barStartTick(&meter, kBar44) == kBar44);
  CHECK(barEndTick(&meter, kBar44) == 2 * kBar44);
  CHECK(barStartTick(&meter, kBar44 + 7) == kBar44);

  // The two are each other's halves: the end of the bar holding a tick is the start of the bar
  // holding that end. And the invariants each guard exists to keep.
  const uint64_t ticks[] = {0, 1, kQ, kBar44 - 1, kBar44, 3 * kBar44 + 12345};
  for (uint64_t t : ticks) {
    CHECK(barStartTick(&meter, barEndTick(&meter, t)) == barEndTick(&meter, t));
    CHECK(barEndTick(&meter, t) > t);
    CHECK(barStartTick(&meter, t) <= t);
  }
}

void testBarTicksAcrossASignatureChange() {
  // 4/4 for two bars, then 7/8. THIS IS THE CASE THE OLD ARITHMETIC GOT WRONG: after the change,
  // bars are no longer at multiples of anything, so `(tick / barLength) * barLength` lands off
  // the ruler the user is reading.
  daw::TimeSignatureMap meter;
  meter.setMap({{0, daw::TimeSignature{4, 4}}, {2 * kBar44, daw::TimeSignature{7, 8}}});

  const uint64_t bar78 = daw::TimeSignature{7, 8}.barNanoticks();  // 7 eighths = 3.5 quarters
  CHECK(bar78 == 7 * (kQ / 2));
  CHECK(bar78 != kBar44);  // otherwise this test proves nothing

  CHECK(barStartTick(&meter, 2 * kBar44) == 2 * kBar44);
  CHECK(barEndTick(&meter, 2 * kBar44) == 2 * kBar44 + bar78);
  CHECK(barStartTick(&meter, 2 * kBar44 + bar78 + 3) == 2 * kBar44 + bar78);
  CHECK(barEndTick(&meter, 2 * kBar44 + bar78 + 3) == 2 * kBar44 + 2 * bar78);

  // AND THE NEGATIVE HALF: the naive 4/4 arithmetic gives a DIFFERENT answer here, which is the
  // whole reason this function exists. If these ever agree, the meter is being ignored again.
  const uint64_t tick = 2 * kBar44 + bar78 + 3;
  CHECK(barStartTick(&meter, tick) != (tick / kBar44) * kBar44);
  CHECK(barEndTick(&meter, tick) != (tick / kBar44 + 1) * kBar44);

  // Bars BEFORE the change are still 4/4 — a later signature must not retroactively renumber.
  CHECK(barStartTick(&meter, kBar44 + 5) == kBar44);
  CHECK(barEndTick(&meter, kBar44 + 5) == 2 * kBar44);
}

void testBarTicksWithNoMeterPublished() {
  // Null is "the snapshot has not been swapped in yet", which happens during startup — not
  // "this project has no meter". A 4/4 bar is what the map itself would have said.
  CHECK(barStartTick(nullptr, 0) == 0);
  CHECK(barEndTick(nullptr, 0) == kBar44);
  CHECK(barStartTick(nullptr, kBar44 + 9) == kBar44);
  CHECK(barEndTick(nullptr, kBar44 + 9) == 2 * kBar44);
}

void testBarTicksSurviveAZeroLengthBar() {
  // A SIGNATURE THAT PASSES valid() AND STILL HAS NO LENGTH. valid() only requires a non-zero
  // power-of-two denominator, and beatNanoticks() is (4 * kNanoticksPerQuarter) / denominator —
  // integer division. Past 2^22 that truncates to zero, so the bar is zero nanoticks long, and
  // then barBeatAt() gives up and answers bar 1 for every tick while tickAtBar() answers 0 for
  // every bar. The denominator comes out of a project file, so this arrives from data rather
  // than from a bug, and both numbers reach these functions looking perfectly ordinary.
  daw::TimeSignature degenerate{4, 1u << 23};
  CHECK(degenerate.valid());  // the map will NOT drop it
  CHECK(degenerate.barNanoticks() == 0);

  daw::TimeSignatureMap meter;
  meter.setMap({{0, degenerate}});

  // The invariants have to hold anyway. Without the guards barEndTick returns 0 for every tick —
  // a note whose default duration is zero and a song span that never grows, which presents as
  // "note entry does nothing" rather than as a bad time signature.
  const uint64_t ticks[] = {0, 1, kQ, kBar44 + 77};
  for (uint64_t t : ticks) {
    CHECK(barEndTick(&meter, t) > t);
    CHECK(barStartTick(&meter, t) <= t);
  }
  CHECK(barEndTick(&meter, kQ) == kQ + kBar44);  // advanced by a fallback bar from the tick
}


void testTheMapNeverPutsABarStartPastItsOwnTick() {
  // THIS TEST EXISTS BECAUSE A NEGATIVE CONTROL PASSED. Deleting barStartTick's
  // `start <= tick` guard changes no result, in any meter tried — so the guard is unreachable,
  // and no test of barStartTick could ever catch its removal. Rather than delete a cheap clamp or
  // leave an unjustifiable line, this pins the PREMISE it rests on: TimeSignatureMap is
  // self-consistent, so the start of the bar barBeatAt() reports is never past the tick asked
  // about.
  //
  // Which makes the guard defence against a FUTURE change to the map, and makes this the test
  // that fires if that change ever lands — saying "the guard just became live" instead of
  // silently letting a new clip anchor after the note that created it.
  //
  // The degenerate maps are the interesting third of these. A zero-length bar makes barBeatAt()
  // give up and answer bar 1, and tickAtBar(1) is 0 by definition, so the premise survives by a
  // different route than it does for a well-formed meter — worth covering explicitly, because
  // those two routes could break independently.
  const uint64_t Q = daw::kNanoticksPerQuarter;
  const std::vector<std::vector<daw::TimeSignaturePoint>> maps = {
      {},
      {{0, {4, 4}}},
      {{0, {3, 4}}},
      {{0, {7, 8}}},
      {{0, {4, 4}}, {8 * Q, {7, 8}}},
      {{0, {7, 8}}, {7 * Q / 2, {3, 4}}, {40 * Q, {5, 16}}},
      {{0, {4, 1u << 23}}},                       // a bar of zero length
      {{0, {4, 4}}, {4 * Q, {4, 1u << 23}}},      // zero-length SECOND segment
      {{0, {4, 1u << 23}}, {4 * Q, {4, 4}}},      // zero-length FIRST segment
      {{0, {1, 64}}, {Q / 8, {32, 1}}},
  };
  int violations = 0;
  for (const auto& pts : maps) {
    daw::TimeSignatureMap m;
    m.setMap(pts);
    for (uint64_t t = 0; t < 64 * Q; t += Q / 16) {
      if (m.tickAtBar(m.barBeatAt(t).bar) > t) {
        ++violations;
      }
      // And the two rules built on it hold for every one of these shapes, degenerate included.
      if (barEndTick(&m, t) <= t) {
        ++violations;
      }
      if (barStartTick(&m, t) > t) {
        ++violations;
      }
    }
  }
  CHECK(violations == 0);
}


// ------------------------------------------- how far a placement reaches, at all three levels
namespace {
daw::ProjectClip clipWith(uint32_t id, uint64_t length) {
  daw::ProjectClip c;
  c.id = id;
  c.lengthNanoticks = length;
  return c;
}
daw::ProjectPlacement placementOf(uint32_t clipId, uint64_t at, uint64_t length) {
  daw::ProjectPlacement p;
  p.clipId = clipId;
  p.at = at;
  p.lengthNanoticks = length;
  return p;
}
}  // namespace

void testPlacementLength() {
  const uint64_t Q = daw::kNanoticksPerQuarter;
  std::vector<daw::ProjectClip> clips = {clipWith(7, 8 * Q)};

  // LEVEL 1: the placement's own length wins, even when the clip says otherwise.
  CHECK(placementLength(placementOf(7, 0, 3 * Q), clips) == 3 * Q);

  // LEVEL 2: zero means "as long as the clip".
  CHECK(placementLength(placementOf(7, 0, 0), clips) == 8 * Q);

  // LEVEL 3: THE ONE FOUR SITES OUT OF FIVE DID NOT HAVE. A clip can hold notes while its own
  // length is still zero — a clip somebody just created and typed into. Without this level the
  // placement measures as EMPTY, and it did: the shared-clip warning went silent on exactly the
  // placement most likely to be edited, because note entry said it covered its content while the
  // published extent said it covered nothing.
  std::vector<daw::ProjectClip> unsized = {clipWith(7, 0)};
  daw::MusicalEvent note;
  note.nanotickOffset = 5 * Q;
  note.type = daw::MusicalEventType::Note;
  note.payload.note.pitch = 60;
  note.payload.note.durationNanoticks = Q;
  unsized[0].clip.addEvent(note);
  const uint64_t content = clipContentEnd(unsized[0].clip);
  CHECK(content > 0);  // otherwise this test proves nothing about level 3
  CHECK(placementLength(placementOf(7, 0, 0), unsized) == content);

  // A zero-length clip with NO content still reaches nowhere — level 3 is a fallback, not a
  // minimum, and inventing a length here would hide an empty clip.
  std::vector<daw::ProjectClip> empty = {clipWith(7, 0)};
  CHECK(placementLength(placementOf(7, 0, 0), empty) == 0);

  // A DANGLING CLIP REFERENCE REACHES NOWHERE. All five hand-written copies did this; a
  // plausible default would hide the dangling reference instead of letting it show up as a
  // placement that covers nothing.
  CHECK(placementLength(placementOf(999, 0, 0), clips) == 0);
  // ...but an explicit length still stands on its own, without needing the clip at all.
  CHECK(placementLength(placementOf(999, 0, 2 * Q), clips) == 2 * Q);
}

void testPlacementReachSaturates() {
  CHECK(placementReach(100, 50) == 150);
  CHECK(placementReach(0, 0) == 0);

  // THE GUARD THREE OF THE FIVE COPIES LACKED. A placement near the top of the range must not
  // wrap: as a song end it would silence everything after it, and in the ripple planner a wrapped
  // end wrongly accepts or refuses a time edit. Note that the wrong answer is SMALL, so it reads
  // as "the song is shorter than I thought" rather than as arithmetic.
  CHECK(placementReach(UINT64_MAX - 10, 100) == UINT64_MAX);
  CHECK(placementReach(UINT64_MAX, 1) == UINT64_MAX);
  CHECK(placementReach(UINT64_MAX, UINT64_MAX) == UINT64_MAX);

  // Exactly at the boundary is not an overflow and must not saturate.
  CHECK(placementReach(UINT64_MAX - 10, 10) == UINT64_MAX);
  CHECK(placementReach(UINT64_MAX - 10, 9) == UINT64_MAX - 1);
}

}  // namespace

// THE HISTORY VERBS: what they CHANGE and whether they open a STEP are two questions, and one
// bool was answering both by lying about the first.
//
// `commandMutatesDocument(Undo)` returned false with the comment "no document state" while
// `handleUndo` calls `applyDocument` and replaces the entire ProjectDocument. G2-A's arbitrated
// population is derived from that predicate, so the inconsistency was in the thing the gate
// depends on. Open item 32, RULED (R11).
//
// THESE ARE constexpr, so the assertions are also static_asserts — a regression cannot compile.
// Written as CHECKs too, because a static_assert that someone deletes leaves nothing behind, and
// the run reports which pairing broke rather than which line failed to compile.
static_assert(daw::engine::commandMutatesDocument(daw::UiCommandType::Undo),
              "Undo replaces the document via applyDocument");
static_assert(daw::engine::commandMutatesDocument(daw::UiCommandType::Redo),
              "Redo replaces the document exactly as Undo does");
static_assert(daw::engine::commandUndoPolicy(daw::UiCommandType::Undo) ==
                  daw::engine::UndoPolicy::None,
              "a step for the undo is how a history eats itself");
static_assert(daw::engine::commandUndoPolicy(daw::UiCommandType::Redo) ==
                  daw::engine::UndoPolicy::None,
              "same for redo");

// THE CAUSAL LINK R11 WAS MISSING. My negative control showed the policy guard is inert on the
// happy path and I wrote that it is not load-bearing — a universal claim from one path.
// codex-worker-1 refuted it: `commit` appends when the document OR THE PLUGIN SNAPSHOT differs, so
// where the cursor's snapshot is PARTIAL and a later capture fills it in, a policy of Version would
// append an Undo-labelled version and DESTROY THE REDO TAIL. UndoPolicy::None prevents that by
// never reaching `commit` at all.
//
// TWO LINKS, EACH TESTABLE, TOGETHER CAUSAL:
//   here            None -> Skip, so `commit` is never called
//   history tests   a partial-snapshot difference alone makes `commit` append and truncate
// The end-to-end version needs a plugin that refuses and then answers, which no fixture provides —
// and reaching the bracket at all needed a 24-struct HandleUiEntryDeps. This is the route backend
// chose over both.
// THE CHAIN PING-PONG, which is what makes `inputPtrs = outputPtrs` safe.
//
// Adjacent hosted plugins hand audio along by REBINDING, not copying. That reads as in-place
// aliasing and is not: the output alternates A/B by parity while the input is the PREVIOUS
// iteration's output, of opposite parity. I raised the aliasing as a hazard in P2-G4-01 and it was
// wrong; this pins the reason it is wrong, because the reason is arithmetic and a "simplification"
// of the parity would still produce working audio for a TWO-plugin chain and break three.
//
// Tested over segment lengths 1..8 rather than one case, because the failure this guards against
// appears at length 3 and not before.
void testChainBuffersPingPong() {
  using daw::host::ChainBuffer;
  using daw::host::ChainInput;
  using daw::host::chainInputFor;
  using daw::host::chainOutputFor;
  using daw::host::outputDiffersFromInput;

  for (uint32_t len = 1; len <= 8; ++len) {
    const uint32_t start = 3;  // non-zero, so the parity is relative to the SEGMENT and not to 0
    const uint32_t end = start + len;
    for (uint32_t i = start; i < end; ++i) {
      // THE PROPERTY: a plugin never writes into the buffer it is reading.
      CHECK(outputDiffersFromInput(i, start, end));
      // AND THE PRE-CLEAR follows the same selection, so zeroing the output can never zero the
      // input. Stated as its own assertion because they were two derivations of one parity until
      // this commit, and two derivations of one fact is how they drift.
      const ChainBuffer out = chainOutputFor(i, start, end);
      const ChainInput in = chainInputFor(i, start, end);
      if (out == ChainBuffer::A) CHECK(in != ChainInput::A);
      if (out == ChainBuffer::B) CHECK(in != ChainInput::B);
    }
    // ONLY THE LAST writes the segment output, and it is the only one that does.
    for (uint32_t i = start; i + 1 < end; ++i) {
      CHECK(chainOutputFor(i, start, end) != ChainBuffer::SegmentOutput);
    }
    CHECK(chainOutputFor(end - 1, start, end) == ChainBuffer::SegmentOutput);
    // THE FIRST reads the segment input and nobody else does.
    CHECK(chainInputFor(start, start, end) == ChainInput::SegmentInput);
    for (uint32_t i = start + 1; i < end; ++i) {
      CHECK(chainInputFor(i, start, end) != ChainInput::SegmentInput);
    }
  }

  // THE SHAPE, SPELLED OUT for a segment of four, so a reader can see the ping-pong rather than
  // infer it from the loop above — and so a wrong-but-consistent selection cannot satisfy both.
  const uint32_t s = 0, e = 4;
  CHECK(chainOutputFor(0, s, e) == ChainBuffer::A);
  CHECK(chainOutputFor(1, s, e) == ChainBuffer::B);
  CHECK(chainOutputFor(2, s, e) == ChainBuffer::A);
  CHECK(chainOutputFor(3, s, e) == ChainBuffer::SegmentOutput);
  CHECK(chainInputFor(0, s, e) == ChainInput::SegmentInput);
  CHECK(chainInputFor(1, s, e) == ChainInput::A);
  CHECK(chainInputFor(2, s, e) == ChainInput::B);
  CHECK(chainInputFor(3, s, e) == ChainInput::A);
}

void testRecordActionSkipsTheHistoryVerbs() {
  using daw::engine::RecordAction;
  using daw::engine::recordActionFor;
  using daw::engine::UndoPolicy;

  // THE LINK ITSELF: a history verb never reaches `commit`, whatever the snapshot holds, and
  // whatever a gesture is doing. `commit` is where a partial snapshot would append.
  for (bool gesture : {false, true}) {
    CHECK(recordActionFor(UndoPolicy::None, /*haveHistory=*/true, /*haveCapture=*/true, gesture) ==
          RecordAction::Skip);
  }
  // AND THE SABOTAGE ARM: with the history verbs mapped to Version — the change a bare bool-flip
  // of commandMutatesDocument would have made — the same inputs reach Commit, which is the call
  // that appends and truncates.
  CHECK(recordActionFor(UndoPolicy::Version, true, true, /*amendForGesture=*/false) ==
        RecordAction::Commit);

  // THE REST OF THE TABLE, so the assertion above is about the POLICY and not about the function
  // returning Skip broadly.
  CHECK(recordActionFor(UndoPolicy::Version, true, true, /*amendForGesture=*/true) ==
        RecordAction::Amend);
  CHECK(recordActionFor(UndoPolicy::Amend, true, true, false) == RecordAction::Amend);
  CHECK(recordActionFor(UndoPolicy::Amend, true, true, true) == RecordAction::Amend);
  // NO HISTORY OR NO CAPTURE IS ALSO SKIP — the two null guards the destructor had inline, kept
  // in the decision rather than left behind in the caller where a second reader would re-derive.
  CHECK(recordActionFor(UndoPolicy::Version, /*haveHistory=*/false, true, false) ==
        RecordAction::Skip);
  CHECK(recordActionFor(UndoPolicy::Version, true, /*haveCapture=*/false, false) ==
        RecordAction::Skip);

  // AND THE POLICY THE HISTORY VERBS ACTUALLY HAVE, so this test fails if someone maps them to
  // Version — which is the regression the whole of R11 is about.
  CHECK(daw::engine::commandUndoPolicy(daw::UiCommandType::Undo) == UndoPolicy::None);
  CHECK(daw::engine::commandUndoPolicy(daw::UiCommandType::Redo) == UndoPolicy::None);
}

void testHistoryVerbsMutateButOpenNoStep() {
  using daw::engine::commandMutatesDocument;
  using daw::engine::commandUndoPolicy;
  using daw::engine::UndoPolicy;

  // THE PREDICATE NOW SAYS WHAT IT NAMES.
  CHECK(commandMutatesDocument(daw::UiCommandType::Undo));
  CHECK(commandMutatesDocument(daw::UiCommandType::Redo));

  // AND BEHAVIOUR IS UNCHANGED, which is the whole point of the separation: the history verbs
  // still open no step. If flipping the bool had been the whole change, Ctrl-Z would have started
  // pushing a step for itself and this is the assertion that would have caught it.
  CHECK(commandUndoPolicy(daw::UiCommandType::Undo) == UndoPolicy::None);
  CHECK(commandUndoPolicy(daw::UiCommandType::Redo) == UndoPolicy::None);

  // THE THREE OTHER ANSWERS STILL LAND WHERE THEY DID, so this change is bounded to the two verbs:
  //   a command that changes nothing saved                     -> None
  //   a command that changes what EXISTS                       -> Version
  //   the audition swap, which changes only what you are hearing -> Amend
  CHECK(!commandMutatesDocument(daw::UiCommandType::TogglePlay));
  CHECK(commandUndoPolicy(daw::UiCommandType::TogglePlay) == UndoPolicy::None);
  CHECK(commandMutatesDocument(daw::UiCommandType::WriteNote));
  CHECK(commandUndoPolicy(daw::UiCommandType::WriteNote) == UndoPolicy::Version);
  CHECK(commandMutatesDocument(daw::UiCommandType::SwapPlacementClip));
  CHECK(commandUndoPolicy(daw::UiCommandType::SwapPlacementClip) == UndoPolicy::Amend);
}

int main() {
  testChainBuffersPingPong();
  testRecordActionSkipsTheHistoryVerbs();
  testHistoryVerbsMutateButOpenNoStep();
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
  testBarTicksIn44();
  testBarTicksAcrossASignatureChange();
  testBarTicksWithNoMeterPublished();
  testBarTicksSurviveAZeroLengthBar();
  testTheMapNeverPutsABarStartPastItsOwnTick();
  testPlacementLength();
  testPlacementReachSaturates();

  if (g_fail == 0) {
    std::printf("engine_pure_tests: PASS\n");
    return 0;
  }
  std::printf("engine_pure_tests: FAIL (%d)\n", g_fail);
  return 1;
}
