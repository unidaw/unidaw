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
#include "engine_transport_state.h"
#include "engine_types.h"
#include "event_payloads.h"
#include "note_entry.h"

namespace daw::engine {

struct LocateTargetDeps {
  std::atomic<uint32_t>& nextClipId;
  std::atomic<uint32_t>& nextPlacementId;
  std::function<daw::BarGrid()> songBarGrid;
};

struct ClipEditDeps {
  std::function<uint64_t(uint64_t)> barEndTick;
  std::function<uint32_t(TrackRuntime*)> bumpClipVersionFor;
  const std::function<uint32_t(uint32_t)>& bumpTrackClipVersion;
  std::atomic<bool>& clipDirty;
  std::atomic<uint32_t>& clipVersion;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&, uint32_t, TrackStoreState&&, bool)> commitStructuralEdit;
  std::function<void(TrackRuntime*)> consumeClipVersionForNoOp;
  std::function<void(const daw::UiChordDiffPayload&)> emitChordDiff;
  std::function<void(const daw::UiDiffPayload&)> emitUiDiff;
  std::function<void(TrackRuntime&, size_t)> forkOwnedClip;
  std::function<void(TrackRuntime&, const EditTarget&)> growLengthsForContent;
  std::function<EditTarget(TrackRuntime&, uint64_t, bool)> locateEditTarget;
  TransportState& transport;
  std::atomic<uint32_t>& nextChordId;
  std::atomic<uint32_t>& nextClipId;
  const uint64_t patternTicks;
  std::function<void(uint32_t, TrackStoreState, TrackStoreState)> pushStructuralUndo;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
  std::function<TrackStoreState(const TrackRuntime&)> snapshotTrackStore;
  TrackTable& trackTable;
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

}  // namespace daw::engine
