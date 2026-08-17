#pragma once
// APPLYING AN EDIT TO A TRACK'S CLIPS — five functions lifted verbatim out of main().
//
// locateEditTarget answers "which clip does this tick belong to, and mint one if none does".
// The other four are the edits that ask it: add a note, add a chord, set a row's trig ops, and
// the local (per-appearance) note edit that writes an override instead of touching the clip.
//
// THEY MOVE TOGETHER BECAUSE THEIR DEPENDENCIES DO. Measured with tools/extraction_cost.sh, the
// five need 10, 10, 14, 15 and 3 captures — 52 in total, but only TWENTY-THREE DISTINCT. Split
// across five modules that would be 52 struct members, 52 call-site arguments and five separate
// interfaces onto the same state. One module, one ClipEditDeps, and the shared surface is stated
// once. 656 lines of main() for 23 names.
//
// Each function binds only the subset it uses, not all 23: this file is compiled with
// -Werror=unused-variable, so a dependency that stops being needed fails the build instead of
// quietly becoming decoration.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "engine_track_table.h"
#include "engine_command_outcome.h"
#include "engine_transport_state.h"
#include "engine_types.h"
#include "event_payloads.h"
#include "note_entry.h"
#include "apps/engine_state.h"

namespace daw::engine {

struct LocateTargetDeps {
  std::atomic<uint32_t>& nextClipId;
  std::atomic<uint32_t>& nextPlacementId;
  std::function<daw::BarGrid()> songBarGrid;
};

struct ClipEditDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  std::function<uint64_t(uint64_t)> barEndTick;
  std::atomic<bool>& clipDirty;
  std::atomic<uint32_t>& clipVersion;
  // ADDED WITH ensurePlacementIds, which hands out placement ids, and with forkOwnedClip, which
  // needs a fresh one when it copies. bumpClipVersionFor, forkOwnedClip and growLengthsForContent
  // were MEMBERS until this commit; they are functions in this file now, so three std::functions
  // and their indirection went away in exchange.
  std::atomic<uint32_t>& nextPlacementId;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&, uint32_t, TrackStoreState&&, bool)> commitStructuralEdit;
  std::function<void(const daw::UiChordDiffPayload&)> emitChordDiff;
  std::function<void(const daw::UiDiffPayload&)> emitUiDiff;
  std::function<EditTarget(TrackRuntime&, uint64_t, bool)> locateEditTarget;
  std::atomic<uint32_t>& nextChordId;
  std::atomic<uint32_t>& nextClipId;
  const uint64_t patternTicks;
  std::function<void(uint32_t, TrackStoreState, TrackStoreState)> pushStructuralUndo;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
  std::function<TrackStoreState(const TrackRuntime&)> snapshotTrackStore;
  // ADDED WITH requireMatchingClipVersion, which REFUSES rather than returning a value: it tells
  // the caller through emitClipReject and records the refusal in the journal, so both arrive here
  // together. clipVersion, emitUiDiff and trackTable were already members.
  std::function<void(daw::UiClipRejectReason, uint32_t, uint32_t, uint32_t,
                           daw::UiCommandType)> emitClipReject;
  std::function<void(const char*, const char*, uint32_t, uint32_t,
                           const std::string&)> historyAppend;
  CommandOutcomePublisher publishCommandOutcome;
};

EditTarget locateEditTarget(LocateTargetDeps& deps, TrackRuntime& rt, uint64_t absTick,
                            bool createIfMissing);

bool applyAddNote(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint64_t duration,
                  uint8_t pitch, uint8_t velocity, uint16_t flags, bool recordUndo,
                  std::optional<daw::EventId> noteIdOverride, uint16_t sound,
                  uint16_t soundOffset);

bool applySetRowOps(ClipEditDeps& deps, uint32_t trackId, uint32_t clipId, daw::EventId noteId,
                    const daw::RowOpEdit& edit, bool recordUndo,
                    daw::UiClipRejectReason& rejectReason);

// A LOCAL EDIT — one that belongs to THIS APPEARANCE of a clip rather than to the clip itself.
// Recorded on the placement as an `add` (a note only this appearance has) or a `mute` (a base note
// only this appearance is missing), which is what makes "fix the bass in chorus 1, all three
// choruses change, and the hat you added to chorus 3 survives" expressible at all: the bass fix is
// a CLIP edit and reaches all three, the hat is a LOCAL edit and stays where it was put.
//
// ADDITIVE-ONLY, ON PURPOSE (roadmap item 24): there is no "changed note" record. An edit that
// would MODIFY a base note is decomposed into mute(original) + add(new), so the override list is
// always a set of things added and things silenced, and reverting is deleting both vectors rather
// than replaying inverses.
bool applyLocalNoteEdit(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick,
                        uint64_t duration, uint8_t pitch, uint8_t velocity, uint8_t column,
                        bool deleting,
                        const std::function<PlacementHit(TrackRuntime&, uint64_t)>&
                            findPlacementAt);

bool applyAddChord(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint64_t duration,
                   uint8_t degree, uint8_t quality, uint8_t inversion, uint8_t baseOctave,
                   uint8_t column, uint32_t spreadNanoticks, uint16_t humanizeTiming,
                   uint16_t humanizeVelocity, bool recordUndo,
                   std::optional<uint32_t> chordIdOverride);
bool applyRemoveNote(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint8_t pitch, uint16_t flags, bool recordUndo);
bool applyRemoveChord(ClipEditDeps& deps, uint32_t trackId, uint32_t chordId, bool recordUndo);
bool applyRemoveChordAt(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint8_t column, bool recordUndo);

// THE OPTIMISTIC-CONCURRENCY GUARD. Every clip edit carries the clipVersion it was composed
// against; if the engine has moved on, the edit is REFUSED rather than applied to a document the
// caller has not seen. That refusal goes to the UI and to the journal, which is why this needs
// both emitClipReject and historyAppend.
// THE COUNTER A COMMAND IS GATED ON, and whether its track is there — one read, two callers.
//
// Track-scoped commands are gated on the track's own `trackClipVersion`; global-scope ones on the
// engine's `clipVersion`, because an undo can touch ANY track and comparing it against the caller's
// incidental trackId would let it ride on track 0's version. Reading that took a mutex and a
// removed-flag check, and it lived INSIDE the version gate — so the row-op refusal, which is not
// version-gated and therefore never calls the gate, had no way to say what the engine holds and
// reported a literal 0 instead. Extracted rather than copied: a second implementation of a read
// under a lock is how the two answers start to disagree.
//
// `out` IS ALWAYS SET TO A REAL VALUE, and that is the whole point of the contract rather than a
// convenience. The first version left `out` untouched when the track was absent and returned false
// — which put the burden on every caller to notice, and the first caller written against it did
// not: it kept its own initialiser of 0 and emitted that, so the MISSING-track refusal reported a
// fabricated 0, which is the defect this function was extracted to end. backend caught it.
//
// When there is no per-track counter the answer is the engine's GLOBAL `clipVersion`, which is what
// the two sibling emit sites in this file have always reported in that case. It is a real figure
// the engine holds, and reporting it is not the same claim as reporting a track's own.
//
// The bool says WHICH counter `out` came from: true = the track's own, false = the global, because
// the track is absent or removed. A caller that ignores it still cannot fabricate.
bool currentClipVersionFor(ClipEditDeps& deps, daw::UiCommandType commandType, uint32_t trackId,
                           uint32_t& out);

bool requireMatchingClipVersion(ClipEditDeps& deps, uint32_t baseVersion, daw::UiCommandType commandType, uint32_t trackId);

// Which placement covers this tick on this track, if any. The one answer to a question that used
// to be asked five different ways — for the same reason editIsLocalScope is one function: the
// scope decision and the target decision have to agree, and they were two separate loops that
// agreed only by accident.
//
// OVERLAPPING PLACEMENTS made both of them arbitrary. Each took the FIRST match in
// sourcePlacements — file order, or insertion order, which is nothing the user can see. Worse,
// they disagreed in a way that mattered: editIsLocalScope scanned for ANY placement under the tick
// with localEdits set, while the target loop took the first containing placement whether its flag
// was set or not. So with two overlapping appearances, one local and one not, the gesture could be
// RULED local and then applied to the placement that is not — an override recorded on an
// appearance the user never marked.
//
// The tie-break is the LATEST START among the placements containing the tick, and on an exact tie
// the later one in the list. "Topmost wins" is the convention every arranger uses for stacked
// material, and stating it is the point: an arbitrary rule that happens to be stable is still
// unpredictable to the person using it.
PlacementHit findPlacementAt(ClipEditDeps& deps, TrackRuntime& rt, uint64_t nanotick);

// COPY-ON-WRITE FOR A SHARED CLIP: editing one instance must not edit the others.
void forkOwnedClip(ClipEditDeps& deps, TrackRuntime& rt, size_t ownedIndex);

// A clip grows to fit what was put in it, rather than truncating the note that overran.
void growLengthsForContent(ClipEditDeps& deps, TrackRuntime& rt, const EditTarget& t);

// Per-track value first, the global gate last — the ORDER is the publisher's contract.
uint32_t bumpClipVersionFor(ClipEditDeps& deps, TrackRuntime* runtime);

// Every track at once, in the same order and for the same reason.
void bumpAllTrackClipVersions(ClipEditDeps& deps);

// Every placement gets an id, and nextPlacementId ends above every id in use.
void ensurePlacementIds(ClipEditDeps& deps, std::vector<daw::ProjectPlacement>& placements);

// Whether this edit touches one instance or every instance of a shared clip.
//
// DOES THIS EDIT BELONG TO THE APPEARANCE OR TO THE CLIP? One function, because WriteNote and
// DeleteNote both have to answer it and two copies would eventually disagree about the same
// gesture — which for this feature means the same keystroke doing different things depending on
// which handler ran.
//
// The explicit bit wins on its own: a caller that SAID which it meant is never overridden. The
// placement's own flag is the standing answer for when nobody said. Never inferred from whether
// the cell is occupied.
bool editIsLocalScope(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint16_t flags);

// Whether this track may edit that clip in place, or must fork it first.
bool isEditableClip(ClipEditDeps& deps, const TrackRuntime& rt, uint32_t id);

// Same bump by track id rather than by pointer. TWO ENTRY POINTS BECAUSE OF LOCK ORDER: code
// already holding a track's trackMutex must not reach for tracksMutex, so those sites pass the
// TrackRuntime* they already hold.
uint32_t bumpTrackClipVersion(ClipEditDeps& deps, uint32_t trackId);

// The UI reserves one clip version per edit it queues, so an edit whose base version matched must
// advance the counter even when the edit turns out to be a no-op — otherwise the UI stays one
// ahead forever and every later edit is refused.
void consumeClipVersionForNoOp(ClipEditDeps& deps, TrackRuntime* runtime);

}  // namespace daw::engine
