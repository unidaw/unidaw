// Tests for apps/engine_track_rebuild.h — rebuilding one track's derived state.
//
// THESE RULES HAD NO DIRECT COVERAGE UNTIL rebuildFlatAndPublish LEFT main(). It was a lambda
// capturing main()'s scope, so asking it a question meant booting an engine, mapping shared memory,
// loading a project and driving a command through a ring. Everything below is now a function call.
//
// The rule under test is the MUTE PRUNE, and it is worth testing because it has a guard that looks
// like an oversight. A per-appearance mute names a note by id; if the note is deleted from the CLIP,
// the mute survives pointing at nothing, and the appearance keeps advertising an override that
// cannot be found or reverted. So the rebuild prunes dead mutes — but ONLY when the referenced clip
// is actually present. With the clip absent, a mute pointing at an unknown id is indistinguishable
// from one whose clip has not been installed yet, and pruning would delete real user edits during
// load. A simplification that drops that condition passes every existing test.
//
// THE GUARD IS LOAD-BEARING TWICE OVER, which the negative control made obvious. Deleting it
// outright SEGFAULTS — the loop then dereferences the null clipDef — so that version fails loudly
// for a reason that has nothing to do with the rule. The interesting sabotage is the plausible one:
// treat an absent clip as proof the mutes are dead and clear them. No crash, no warning, correct-
// looking code, and testAbsentClipLeavesMutesAlone below is what catches it.
#include "apps/engine_track_rebuild.h"

#include <cstdio>
#include <vector>

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

constexpr uint64_t kQ = 960000;
constexpr uint64_t kBar = 4 * kQ;

daw::MusicalEvent note(uint64_t tick, uint8_t pitch, uint32_t id) {
  daw::MusicalEvent e;
  e.nanotickOffset = tick;
  e.type = daw::MusicalEventType::Note;
  e.payload.note.pitch = pitch;
  e.payload.note.velocity = 100;
  e.payload.note.column = 0;
  e.payload.note.durationNanoticks = kQ / 2;
  e.payload.note.noteId = id;
  return e;
}

// The two dependencies, answered as plainly as possible: no quantize, and a window that ends
// past everything. Neither is what is under test.
FlatRebuildDeps makeDeps() {
  FlatRebuildDeps deps{
      [](const TrackRuntime&) { return daw::LaneQuantize{}; },
      [](const TrackRuntime&) -> uint64_t { return 64 * kBar; }};
  return deps;
}

// One track holding one clip (id 1, containing note id 1) placed once at 0.
void buildTrack(TrackRuntime& rt, bool installClip, const std::vector<daw::EventId>& mutes) {
  rt.trackId = 0;
  if (installClip) {
    daw::ProjectClip c;
    c.id = 1;
    c.lengthNanoticks = kBar;
    c.kind = daw::ClipKind::Symbolic;
    c.clip.addEvent(note(0, 60, 1));
    rt.ownedClips.push_back(std::move(c));
  }
  daw::ProjectPlacement pl;
  pl.clipId = 1;
  pl.id = 7;
  pl.at = 0;
  pl.lengthNanoticks = kBar;
  pl.mutes = mutes;
  rt.sourcePlacements.push_back(std::move(pl));
}

// ---------------------------------------------- a mute whose note still exists is left alone
void testLiveMuteSurvives() {
  TrackRuntime rt;
  buildTrack(rt, /*installClip=*/true, {1});
  auto deps = makeDeps();
  rebuildFlatAndPublish(deps, rt);
  CHECK(rt.sourcePlacements.size() == 1);
  CHECK(rt.sourcePlacements[0].mutes.size() == 1);
  CHECK(rt.sourcePlacements[0].mutes[0] == 1);
}

// ------------------------------------- a mute pointing at a deleted note is pruned, once, here
void testDeadMuteIsPruned() {
  TrackRuntime rt;
  buildTrack(rt, /*installClip=*/true, {1, 999});
  auto deps = makeDeps();
  rebuildFlatAndPublish(deps, rt);
  CHECK(rt.sourcePlacements[0].mutes.size() == 1);
  CHECK(rt.sourcePlacements[0].mutes[0] == 1);
}

// ------------------------------- BUT NOT WHEN THE CLIP IS ABSENT. This is the guard, and it is
// the whole reason the prune is safe to run on every rebuild: during load a placement can name a
// clip that has not been installed yet, and every mute on it would look dead.
void testAbsentClipLeavesMutesAlone() {
  TrackRuntime rt;
  buildTrack(rt, /*installClip=*/false, {1, 999});
  auto deps = makeDeps();
  rebuildFlatAndPublish(deps, rt);
  CHECK(rt.sourcePlacements[0].mutes.size() == 2);
}

// ------------------------------------------- the published extent counts what SURVIVED the prune
void testExtentCountsPrunedOverrides() {
  TrackRuntime rt;
  buildTrack(rt, /*installClip=*/true, {1, 998, 999});
  rt.sourcePlacements[0].adds.push_back(note(kQ, 64, 500));
  auto deps = makeDeps();
  rebuildFlatAndPublish(deps, rt);
  CHECK(rt.clipExtents.size() == 1);
  // one surviving mute + one add. Counting before the prune would say four.
  CHECK(rt.clipExtents[0].overrideCount == 2);
  CHECK(rt.clipExtents[0].endTick == kBar);
}

}  // namespace

int main() {
  testLiveMuteSurvives();
  testDeadMuteIsPruned();
  testAbsentClipLeavesMutesAlone();
  testExtentCountsPrunedOverrides();
  if (g_fail == 0) {
    std::printf("engine_track_rebuild_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
