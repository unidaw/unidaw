#pragma once
// SNAPSHOT, RESTORE, UNDO — the three operations that treat the whole song as one value.
//
// Undo in this engine is not a log of inverse edits; it is a STORE SWAP. snapshotSongStore takes
// everything a structural edit can touch — clips, placements, chords, harmony, tempo — restore
// puts a whole one back, and applyUndoEntry decides which of the two an entry needs. That is why
// these four move together: they are one idea in four functions, and the pieces they touch are
// exactly the pieces the note store is made of.
//
// WHY THIS COST TWENTY-FOUR CAPTURES AND SEVENTEEN MEMBERS. Restoring a song touches harmony,
// clips, chords, automation and tempo at once, which is inherent — an undo that restored some of
// them would be worse than no undo. Six of those captures collapse into HarmonyTimeline and two
// into TrackTable, which is the whole argument for engine objects: the same body, half the
// interface, because the state that always travels together now travels as one thing.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "engine_arrange_rail.h"
#include "engine_clip_edit.h"
#include "engine_harmony_timeline.h"
#include "engine_song_timing.h"
#include "engine_track_table.h"
#include "engine_types.h"

namespace daw::engine {

// The std::function members are held BY VALUE, as ClipEditDeps and the other command structs do:
// main() passes bare lambdas, and a const reference member would bind to a temporary that dies at
// the end of the initialising statement.
struct SongStoreDeps {
  ArrangeRail& arrange;
  std::atomic<uint32_t>& automationVersion;
  ClipEditDeps& clipEditDeps;
  std::atomic<bool>& clipDirty;
  // Six former captures in one: harmonyEvents, harmonyMutex, harmonyVersion, harmonyDirty,
  // addOrUpdateHarmony and removeHarmony are all its members.
  HarmonyTimeline& harmonyTimeline;
  SongTiming& songTiming;
  daw::TempoMapProvider& tempoProvider;
  TrackTable& trackTable;

  // applyAddNote and applyAddChord are NOT members. main() held them as lambdas that forward to
  // daw::engine::applyAddNote/applyAddChord with clipEditDeps — and clipEditDeps is already a
  // member here, so this file calls those free functions directly. Two std::functions and a layer
  // of indirection went away rather than moving.
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::function<uint32_t(TrackRuntime*)> bumpClipVersionFor;
  std::function<void(std::vector<daw::ProjectPlacement>&)> ensurePlacementIds;
  std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)> rebuildAudioRender;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
  std::function<void()> recomputeSongEnd;
  std::function<std::vector<TrackRuntime*>()> snapshotTracks;
  // A restore tells the UI its clip version moved, which is what stops the next edit from being
  // refused as stale — the same resync the version guard asks for when it refuses one.
  std::function<void(uint32_t, uint32_t)> emitClipResync;
};

// Everything a structural edit can touch, as one value.
SongStoreState snapshotSongStore(SongStoreDeps& deps);

// Puts a whole store back. Returns false if it could not.
bool restoreSongStore(SongStoreDeps& deps, const SongStoreState& state);

// One track's half of a restore; restoreSongStore's inner loop, and its own caller elsewhere.
bool restoreTrackStore(SongStoreDeps& deps, uint32_t trackId, const TrackStoreState& state);

// Which of the two an undo entry needs, and applies it.
bool applyUndoEntry(SongStoreDeps& deps, const daw::UndoEntry& entry, bool redo);

}  // namespace daw::engine
