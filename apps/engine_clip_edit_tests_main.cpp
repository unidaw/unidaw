// Tests for locateEditTarget in apps/engine_clip_edit.h — "which clip does this tick belong to,
// and mint one if none does".
//
// This decides what every note entry, chord entry and row edit acts on, and until it left main()
// it could only be asked by booting an engine and typing into a shared-memory ring. It needs three
// dependencies, so here it is a function call.
//
// The rule it carries that is easiest to break: a REMOVE that lands outside every placement must
// do nothing. `createIfMissing` is false on that path precisely so a failed delete does not mint an
// empty clip and a placement to hold it. Drop the check and every delete on empty space silently
// grows the project — no error, no sound, just clips accumulating where the user pressed backspace.
#include "apps/engine_clip_edit.h"

#include <atomic>
#include <cstdio>

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

// The song's bar grid, answered as plain 4/4 — the anchor rule is not what is under test here.
daw::BarGrid fourFour() {
  return daw::BarGrid{[](uint64_t tick) { return tick - (tick % kBar); }};
}

struct Fixture {
  std::atomic<uint32_t> nextClipId{10};
  std::atomic<uint32_t> nextPlacementId{100};
  TrackRuntime rt;

  LocateTargetDeps deps() {
    return LocateTargetDeps{nextClipId, nextPlacementId, [] { return fourFour(); }};
  }

  // One 1-bar clip placed at bar 0.
  void oneClipAtOrigin() {
    daw::ProjectClip c;
    c.id = 1;
    c.lengthNanoticks = kBar;
    c.kind = daw::ClipKind::Symbolic;
    c.linesPerBeat = 6;              // deliberately NOT the default, so inheritance is visible
    c.timeSigNumerator = 7;
    c.timeSigDenominator = 8;
    rt.ownedClips.push_back(std::move(c));
    daw::ProjectPlacement pl;
    pl.clipId = 1;
    pl.id = 7;
    pl.at = 0;
    pl.lengthNanoticks = kBar;
    rt.sourcePlacements.push_back(std::move(pl));
  }
};

// ------------------------------------------------- a tick inside a placement finds that placement
void testInsidePlacement() {
  Fixture f;
  f.oneClipAtOrigin();
  auto d = f.deps();
  const EditTarget t = locateEditTarget(d, f.rt, kQ, /*createIfMissing=*/true);
  CHECK(t.valid);
  CHECK(t.clipId == 1);
  CHECK(t.relTick == kQ);
  CHECK(t.placementAt == 0);
  // Nothing was minted.
  CHECK(f.rt.ownedClips.size() == 1);
  CHECK(f.rt.sourcePlacements.size() == 1);
  CHECK(f.nextClipId.load() == 10);
}

// -------------------------------------- a REMOVE far outside every placement must mint NOTHING
void testRemoveOutsideCreatesNothing() {
  Fixture f;
  f.oneClipAtOrigin();
  auto d = f.deps();
  const EditTarget t = locateEditTarget(d, f.rt, 40 * kBar, /*createIfMissing=*/false);
  CHECK(!t.valid);
  CHECK(f.rt.ownedClips.size() == 1);
  CHECK(f.rt.sourcePlacements.size() == 1);
  CHECK(f.nextClipId.load() == 10);        // the id counter did not move either
  CHECK(f.nextPlacementId.load() == 100);
}

// ------------------------------------ an ENTRY far outside every placement mints clip + placement
void testEntryOutsideCreates() {
  Fixture f;
  f.oneClipAtOrigin();
  auto d = f.deps();
  const EditTarget t = locateEditTarget(d, f.rt, 40 * kBar, /*createIfMissing=*/true);
  CHECK(t.valid);
  CHECK(f.rt.ownedClips.size() == 2);
  CHECK(f.rt.sourcePlacements.size() == 2);
  CHECK(t.clipId == 10);                   // consumed from nextClipId
  CHECK(f.nextClipId.load() == 11);
  CHECK(f.nextPlacementId.load() == 101);
  // The new clip is editable, which is what lets the note actually land in it.
  CHECK(f.rt.editableClipIds.size() == 1);
  CHECK(f.rt.editableClipIds[0] == 10);
}

// ------------------- and it inherits the PREDECESSOR's grid rather than snapping back to 4/4
void testNewClipInheritsPredecessorGrid() {
  Fixture f;
  f.oneClipAtOrigin();                     // predecessor is 7/8, 6 lines per beat
  auto d = f.deps();
  const EditTarget t = locateEditTarget(d, f.rt, 40 * kBar, /*createIfMissing=*/true);
  CHECK(t.valid);
  const auto& made = f.rt.ownedClips.back();
  CHECK(made.linesPerBeat == 6);
  CHECK(made.timeSigNumerator == 7);
  CHECK(made.timeSigDenominator == 8);
}

}  // namespace

int main() {
  testInsidePlacement();
  testRemoveOutsideCreatesNothing();
  testEntryOutsideCreates();
  testNewClipInheritsPredecessorGrid();
  if (g_fail == 0) {
    std::printf("engine_clip_edit_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
