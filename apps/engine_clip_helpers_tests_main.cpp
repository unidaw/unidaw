// Tests for the small helpers in apps/engine_clip_edit.h — version bumps, placement ids,
// copy-on-write and edit scope.
//
// THESE WERE LAMBDAS INSIDE main() UNTIL NOW, so nothing could reach them without starting an
// engine, and every check that does starts one and asserts on what the UI ends up seeing. That
// kind of check cannot distinguish "the version was bumped in the right order" from "the version
// was bumped", and the ORDER is the whole contract of bumpClipVersionFor.
//
// FOUR INVARIANTS, EACH OF WHICH HAS A FAILURE MODE THAT IS SILENT AT THE UI:
//
//   THE BUMP ORDER. The publisher GATES on the global counter and PUBLISHES the per-track one. If
//   the global moved first, a publish landing between the two increments would latch the new gate
//   while writing the OLD per-track version — and then return early forever after, because the gate
//   already matches. That track's published base would be permanently one behind and every edit
//   against it refused. Value first, gate second.
//
//   PLACEMENT IDS. ensurePlacementIds must leave nextPlacementId strictly above every id in use,
//   INCLUDING ids that arrived from a loaded project. Handing out an id that a placement already
//   has does not fail loudly; it makes two placements indistinguishable.
//
//   COPY-ON-WRITE. forkOwnedClip gives a shared clip a fresh id and repoints this track's
//   placements at it. A fork that repointed nothing would edit every instance; a fork that ran on
//   an ALREADY editable clip would orphan the placements pointing at the old id.
//
//   EDIT SCOPE. kUiEditScopeLocal forces local, and otherwise the answer comes from the chosen
//   placement's own localEdits flag — the same placement the edit will target, so the scope
//   decision and the target decision cannot disagree.
#include "apps/engine_clip_edit.h"
#include "apps/engine_rowops_commands.h"

#include <cstring>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
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

// The minimum ClipEditDeps these four helpers touch. The rest are never called on these paths, so
// they are left default-constructed std::functions — calling one would throw, which is a louder
// failure than a stub that quietly returns nothing.
struct Fixture {
  daw::engine::EngineState engineState;
  TrackTable& trackTable = engineState.trackTable;
  TransportState& transport = engineState.transport;
  std::atomic<bool> clipDirty{false};
  std::atomic<uint32_t> clipVersion{0};
  std::atomic<uint32_t> nextPlacementId{1};
  std::atomic<uint32_t> nextChordId{1};
  std::atomic<uint32_t> nextClipId{100};

  std::function<void(daw::UiClipRejectReason, uint32_t, uint32_t, uint32_t, daw::UiCommandType)>
      emitClipReject = [](daw::UiClipRejectReason, uint32_t, uint32_t, uint32_t,
                          daw::UiCommandType) {};
  std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>
      historyAppend = [](const char*, const char*, uint32_t, uint32_t, const std::string&) {};

  ClipEditDeps deps() {
    return ClipEditDeps{engineState,      // engineState
                        {},               // barEndTick
                        clipDirty,        // clipDirty
                        clipVersion,      // clipVersion
                        nextPlacementId,  // nextPlacementId
                        {},               // commitStructuralEdit
                        {},               // emitChordDiff
                        {},               // emitUiDiff
                        {},               // locateEditTarget
                        nextChordId,      // nextChordId
                        nextClipId,       // nextClipId
                        0,                // patternTicks
                        {},               // pushStructuralUndo
                        {},               // rebuildFlatAndPublish
                        {},               // snapshotTrackStore
                        emitClipReject,   // emitClipReject
                        historyAppend};   // historyAppend
  }

  // A TRACK ID IS ITS INDEX. trackAt() returns tracks[trackId], so a fixture that merely
  // push_back()s a runtime with trackId 7 builds a table where track 7 cannot be found — which
  // reads exactly like a bug in the code under test. The engine grows the vector; so does this.
  TrackRuntime& addTrack(uint32_t trackId) {
    std::lock_guard<std::mutex> lock(trackTable.tracksMutex);
    while (trackTable.tracks.size() <= trackId) {
      auto filler = std::make_unique<TrackRuntime>();
      filler->trackId = static_cast<uint32_t>(trackTable.tracks.size());
      trackTable.tracks.push_back(std::move(filler));
    }
    TrackRuntime& ref = *trackTable.tracks[trackId];
    ref.trackId = trackId;
    return ref;
  }
};

// THE ORDER ITSELF, WHICH THE VALUE CHECKS ABOVE CANNOT SEE. Both orderings leave the same two
// numbers behind, so a single-threaded assertion on the results is blind to the thing the comment
// in bumpClipVersionFor is about. This runs an OBSERVER doing exactly what the publisher does —
// read the gate, then read the per-track value — and asserts the value is never behind the gate.
//
// With the value bumped first and released, an observer that sees gate == k must find value >= k.
// With the gate bumped first, it can see gate == k while the value is still k-1, which is the
// stale base that gets latched and never corrected.
void testBumpOrderIsObservable() {
  Fixture f;
  TrackRuntime& rt = f.addTrack(0);
  auto d = f.deps();

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> violations{0};
  std::atomic<uint64_t> samples{0};
  std::thread observer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const uint32_t gate = f.clipVersion.load(std::memory_order_acquire);
      const uint32_t value = rt.trackClipVersion.load(std::memory_order_acquire);
      if (value < gate) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
      samples.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (int i = 0; i < 200000; ++i) {
    bumpClipVersionFor(d, &rt);
  }
  stop.store(true, std::memory_order_release);
  observer.join();

  // The observer must actually have looked. A loop that never ran would report zero violations
  // and be indistinguishable from a correct order.
  CHECK(samples.load() > 1000);
  CHECK(violations.load() == 0);
  if (violations.load() != 0) {
    std::printf("      %llu observations of a gate ahead of its value, out of %llu\n",
                (unsigned long long)violations.load(), (unsigned long long)samples.load());
  }
}

// ------------------------------------------------------------------ the bump order
void testBumpOrder() {
  Fixture f;
  TrackRuntime& rt = f.addTrack(0);
  auto d = f.deps();

  CHECK(rt.trackClipVersion.load() == 0u);
  CHECK(f.clipVersion.load() == 0u);

  // The RETURN is the per-track value, because that is what a caller publishes.
  const uint32_t returned = bumpClipVersionFor(d, &rt);
  CHECK(returned == 1u);
  CHECK(rt.trackClipVersion.load() == 1u);
  CHECK(f.clipVersion.load() == 1u);

  // Both advance every time, and by one.
  CHECK(bumpClipVersionFor(d, &rt) == 2u);
  CHECK(rt.trackClipVersion.load() == 2u);
  CHECK(f.clipVersion.load() == 2u);

  // A NULL runtime still moves the global gate, and returns IT rather than 0 — the caller has no
  // per-track value to publish in that case.
  CHECK(bumpClipVersionFor(d, nullptr) == 3u);
  CHECK(f.clipVersion.load() == 3u);
  CHECK(rt.trackClipVersion.load() == 2u);  // untouched
}

// The by-id entry point resolves the track and does the same thing. It exists because code holding
// a track's own mutex must not reach for tracksMutex.
void testBumpByTrackId() {
  Fixture f;
  TrackRuntime& rt = f.addTrack(7);
  auto d = f.deps();
  CHECK(bumpTrackClipVersion(d, 7) == 1u);
  CHECK(rt.trackClipVersion.load() == 1u);
  // An unknown track resolves to nullptr, which still advances the global gate and returns it.
  const uint32_t before = f.clipVersion.load();
  CHECK(bumpTrackClipVersion(d, 999) == before + 1);
}

void testBumpAllTracks() {
  Fixture f;
  TrackRuntime& a = f.addTrack(0);
  TrackRuntime& b = f.addTrack(1);
  auto d = f.deps();
  bumpAllTrackClipVersions(d);
  CHECK(a.trackClipVersion.load() == 1u);
  CHECK(b.trackClipVersion.load() == 1u);
  // The global gate moves too, or a publisher would never look.
  CHECK(f.clipVersion.load() >= 1u);
}

// A no-op edit must still consume the version the UI reserved for it.
void testConsumeForNoOp() {
  Fixture f;
  TrackRuntime& rt = f.addTrack(0);
  auto d = f.deps();
  consumeClipVersionForNoOp(d, &rt);
  CHECK(rt.trackClipVersion.load() == 1u);
  CHECK(f.clipVersion.load() == 1u);
}

// ------------------------------------------------------------------- placement ids
void testEnsurePlacementIds() {
  Fixture f;
  auto d = f.deps();
  std::vector<daw::ProjectPlacement> placements(3);
  placements[0].id = 0;   // needs one
  placements[1].id = 40;  // arrived from a project file
  placements[2].id = 0;   // needs one

  ensurePlacementIds(d, placements);

  CHECK(placements[1].id == 40u);           // an id that exists is never reassigned
  CHECK(placements[0].id != 0u);
  CHECK(placements[2].id != 0u);
  CHECK(placements[0].id != placements[2].id);
  // THE INVARIANT: the counter ends above every id in use, so the next allocation cannot collide
  // with the one that came out of the file.
  CHECK(placements[0].id > 40u);
  CHECK(placements[2].id > 40u);
  CHECK(f.nextPlacementId.load() > placements[0].id);
  CHECK(f.nextPlacementId.load() > placements[2].id);

  // Running it again changes nothing — every placement already has an id.
  const uint32_t after = f.nextPlacementId.load();
  const uint32_t id0 = placements[0].id, id2 = placements[2].id;
  ensurePlacementIds(d, placements);
  CHECK(placements[0].id == id0);
  CHECK(placements[2].id == id2);
  CHECK(f.nextPlacementId.load() == after);
}

// ------------------------------------------------------------------ copy on write
void testForkOwnedClip() {
  Fixture f;
  TrackRuntime& rt = f.addTrack(0);
  daw::ProjectClip clip;
  clip.id = 5;
  rt.ownedClips.push_back(clip);
  daw::ProjectPlacement here, elsewhere;
  here.clipId = 5;
  elsewhere.clipId = 6;  // a different clip on the same track must NOT be repointed
  rt.sourcePlacements = {here, elsewhere};
  auto d = f.deps();

  CHECK(!isEditableClip(d, rt, 5));
  forkOwnedClip(d, rt, 0);

  CHECK(rt.ownedClips[0].id != 5u);              // a fresh id
  const uint32_t forked = rt.ownedClips[0].id;
  CHECK(rt.sourcePlacements[0].clipId == forked);  // this track's placement follows it
  CHECK(rt.sourcePlacements[1].clipId == 6u);      // the other one does not
  CHECK(isEditableClip(d, rt, forked));            // and it is editable in place now

  // A SECOND FORK IS A NO-OP, which matters: forking an already-editable clip would give it yet
  // another id and orphan the placements now pointing at this one.
  forkOwnedClip(d, rt, 0);
  CHECK(rt.ownedClips[0].id == forked);
  CHECK(rt.sourcePlacements[0].clipId == forked);

  // An index past the end returns without touching anything.
  forkOwnedClip(d, rt, 99);
  CHECK(rt.ownedClips.size() == 1u);
  CHECK(rt.ownedClips[0].id == forked);
}

// ------------------------------------------------------------------- edit scope
void testEditScope() {
  Fixture f;
  TrackRuntime& rt = f.addTrack(0);
  daw::ProjectPlacement pl;
  pl.id = 1;
  pl.clipId = 5;
  pl.at = 0;  // ProjectPlacement carries an optional start, not a bare tick
  pl.lengthNanoticks = 1000;
  pl.localEdits = false;
  rt.sourcePlacements = {pl};
  auto d = f.deps();

  // The flag forces local regardless of anything else — including a track that does not exist.
  CHECK(editIsLocalScope(d, 0, 100, daw::kUiEditScopeLocal));
  CHECK(editIsLocalScope(d, 999, 100, daw::kUiEditScopeLocal));

  // Without the flag the answer is the CHOSEN placement's own localEdits.
  CHECK(!editIsLocalScope(d, 0, 100, 0));
  rt.sourcePlacements[0].localEdits = true;
  CHECK(editIsLocalScope(d, 0, 100, 0));

  // A tick no placement covers is not local: there is no appearance to scope the edit to.
  CHECK(!editIsLocalScope(d, 0, 999999, 0));
  // An unknown track is not local either, and must not crash.
  CHECK(!editIsLocalScope(d, 999, 100, 0));
}

// WHAT THE ENGINE HOLDS, for a caller that is NOT version-gated.
//
// `handleSetRowOps` passed a literal 0 as `ClipRejected.currentBase` — the field the payload calls
// "the value to retry with" — while every other emit site passes a real figure. Zero is a LIVE
// value: `clipVersion` initialises to 0, so a refusal on a track at version 7 was indistinguishable
// from a genuine base on a fresh track. Open item 29, measured at the frozen product.
//
// The read lived INSIDE the version gate, which a row-op edit never calls, so this asserts the
// extracted helper directly: the negative control is that a track at a NON-ZERO version must report
// that version, which is exactly the case a hardcoded 0 satisfies by accident when the version is 0.
void testCurrentClipVersionForReportsTheTrackCounter() {
  Fixture f;
  auto deps = f.deps();
  TrackRuntime& rt = f.addTrack(3);
  rt.trackClipVersion.store(7, std::memory_order_release);
  f.clipVersion.store(41, std::memory_order_release);

  uint32_t out = 999;
  CHECK(currentClipVersionFor(deps, daw::UiCommandType::SetRowOps, 3, out));
  CHECK(out == 7u);        // the TRACK's counter, not the global one and not a literal 0

  // A GLOBAL-SCOPE command rides the engine counter, because an undo can touch any track.
  out = 999;
  CHECK(currentClipVersionFor(deps, daw::UiCommandType::Undo, 3, out));
  CHECK(out == 41u);

  // A MISSING TRACK STILL GETS A REAL FIGURE — the engine's GLOBAL counter — and the bool says it
  // is not the track's own. THIS ASSERTED `out == 999` (untouched) IN THE FIRST VERSION, and that
  // was the wrong contract: it made "no answer" indistinguishable from the caller's initialiser,
  // and the caller's initialiser was 0. backend caught the consequence at the handler.
  out = 999;
  CHECK(!currentClipVersionFor(deps, daw::UiCommandType::SetRowOps, 9, out));
  CHECK(out == 41u);

  // A REMOVED track is gone for this purpose too — the version gate has always treated it so,
  // and the extraction has to keep that or the two callers disagree about what "present" means.
  rt.removed.store(true, std::memory_order_release);
  out = 999;
  CHECK(!currentClipVersionFor(deps, daw::UiCommandType::SetRowOps, 3, out));
  CHECK(out == 41u);
}

// THE REFUSAL ITSELF, through handleSetRowOps, so the wiring is covered and not just the helper.
// A test of the helper alone would stay green if the row-op site kept its hardcoded 0.
// EVERY REFUSAL REASON, AND BOTH TRACK STATES. The first version of this test drove ONE reason on a
// track its own fixture had ADDED — so the `UnknownTrack` case ran with the track PRESENT, which is
// the one thing that case does not have. It passed while the handler still fabricated a 0 on the
// real missing-track path, because the fixture supplied the precondition the failing case lacks.
// backend found the hole. The refusal is driven per reason, and the track is present or absent as
// the reason implies.
void runRowOpsRefusal(Fixture& f, ClipEditDeps& deps, uint32_t trackId,
                      daw::UiClipRejectReason reason, uint32_t& sawCurrent, uint32_t& sawSent,
                      daw::UiClipRejectReason& sawReason) {
  sawCurrent = 999;
  sawSent = 999;
  sawReason = daw::UiClipRejectReason::None;
  daw::engine::RowopsCommandDeps rowops{
      [reason](uint32_t, uint32_t, daw::EventId, const daw::RowOpEdit&, bool,
               daw::UiClipRejectReason& out) {
        out = reason;
        return false;
      },
      [&](daw::UiClipRejectReason r, uint32_t, uint32_t sentBase, uint32_t currentBase,
          daw::UiCommandType) {
        sawReason = r;
        sawSent = sentBase;
        sawCurrent = currentBase;
      },
      [&](uint32_t t, uint32_t& out) {
        return currentClipVersionFor(deps, daw::UiCommandType::SetRowOps, t, out);
      }};

  daw::UiSetRowOpsPayload p{};
  p.trackId = trackId;
  p.clipId = 1;
  daw::EventEntry entry{};
  std::memcpy(entry.payload, &p, sizeof(p));
  daw::UiCommandPayload header{};
  handleSetRowOps(rowops, entry, header, daw::UiCommandType::SetRowOps);
}

// THE REFUSAL REASON FROM PRODUCTION, not injected. The tests below stubbed applySetRowOps and
// handed it the reason they wanted — so they asserted what the HANDLER does with a reason and never
// what the ENGINE decides one is. That hid the tombstone defect completely: a removed track really
// yields UnknownNote from production, because `trackAt` returns the tombstone and the search over
// the cleared runtime finds nothing, while the injected test said UnknownTrack. codex-worker-1 and
// backend both named it. This drives the real applySetRowOps.
void testRowOpsReasonsComeFromProduction() {
  Fixture f;
  auto deps = f.deps();
  TrackRuntime& rt = f.addTrack(2);
  rt.trackClipVersion.store(5, std::memory_order_release);

  daw::RowOpEdit edit{};
  daw::UiClipRejectReason reason = daw::UiClipRejectReason::None;

  // A TRACK THAT WAS NEVER ADDED.
  reason = daw::UiClipRejectReason::None;
  CHECK(!applySetRowOps(deps, /*trackId=*/9, /*clipId=*/1, /*noteId=*/1, edit,
                        /*recordUndo=*/false, reason));
  CHECK(reason == daw::UiClipRejectReason::UnknownTrack);

  // A REMOVED TRACK. Its slot is TOMBSTONED and kept, so `trackAt` hands back a live pointer to a
  // cleared runtime — this reported UnknownNote before `liveTrackAt`, telling the caller their
  // note was gone when what was gone was the track.
  rt.removed.store(true, std::memory_order_release);
  reason = daw::UiClipRejectReason::None;
  CHECK(!applySetRowOps(deps, /*trackId=*/2, /*clipId=*/1, /*noteId=*/1, edit,
                        /*recordUndo=*/false, reason));
  CHECK(reason == daw::UiClipRejectReason::UnknownTrack);
}

void testRowOpsRefusalReportsTheEngineVersion() {
  Fixture f;
  auto deps = f.deps();
  TrackRuntime& rt = f.addTrack(2);
  rt.trackClipVersion.store(5, std::memory_order_release);
  f.clipVersion.store(41, std::memory_order_release);

  uint32_t sawCurrent = 999, sawSent = 999;
  daw::UiClipRejectReason sawReason = daw::UiClipRejectReason::None;

  // A PRESENT TRACK reports its OWN counter, whatever the reason is.
  for (auto reason : {daw::UiClipRejectReason::UnknownNote,
                      daw::UiClipRejectReason::ValueOutOfRange}) {
    runRowOpsRefusal(f, deps, 2, reason, sawCurrent, sawSent, sawReason);
    CHECK(sawReason == reason);
    CHECK(sawCurrent == 5u);   // was a literal 0 before this change
    CHECK(sawSent == 0u);
  }

  // A MISSING TRACK — the case `UnknownTrack` actually names, and the one the first fix still
  // fabricated a 0 for because the handler ignored the helper's false. Track 9 was never added.
  runRowOpsRefusal(f, deps, 9, daw::UiClipRejectReason::UnknownTrack, sawCurrent, sawSent,
                   sawReason);
  CHECK(sawReason == daw::UiClipRejectReason::UnknownTrack);
  CHECK(sawCurrent == 41u);    // the engine's global counter, and NOT a fabricated 0
  CHECK(sawSent == 0u);

  // A REMOVED TRACK is the same case arriving a different way, and it is the one a live session
  // reaches: the track existed when the edit was sent and was gone by the time it landed.
  rt.removed.store(true, std::memory_order_release);
  runRowOpsRefusal(f, deps, 2, daw::UiClipRejectReason::UnknownTrack, sawCurrent, sawSent,
                   sawReason);
  CHECK(sawCurrent == 41u);
  CHECK(sawSent == 0u);

  // `sentBase` is 0 BY CONTRACT throughout: UiSetRowOpsPayload carries no base version, because a
  // row-op edit is not version-gated. Asserted so that giving it an invented value is a test
  // failure and not a quiet improvement — item 29's remaining half is that the correlation key is
  // inert, which is a decision about the wire and not something this site may make up.
}

}  // namespace

int main() {
  testBumpOrder();
  testBumpOrderIsObservable();
  testBumpByTrackId();
  testBumpAllTracks();
  testConsumeForNoOp();
  testEnsurePlacementIds();
  testForkOwnedClip();
  testEditScope();
  testCurrentClipVersionForReportsTheTrackCounter();
  testRowOpsReasonsComeFromProduction();
  testRowOpsRefusalReportsTheEngineVersion();

  if (g_fail != 0) {
    std::printf("engine_clip_helpers_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_clip_helpers_tests: PASS\n");
  return 0;
}
