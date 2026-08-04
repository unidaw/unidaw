#pragma once

// PLACEMENT COMMANDS: fork/swap a placement's clip, set its edit scope, revert its overrides,
// and move it.
//
// Four arms sharing one idea — an edit addressed at a PLACEMENT rather than at a clip or a track —
// and, until this commit, 346 lines of it inline in handleUiEntry.
//
// GROUPED BY THE UNION OF THEIR CAPTURES, which is the rule this repo arrived at over six
// extractions: taken separately these four need far more dependencies than they do together,
// because they are four views onto the same placement state. One module and one deps struct
// instead of four interfaces onto it.
//
// Bodies moved VERBATIM and diffed line-for-line against the arms they came from. See
// apps/engine_sampler_commands.h for the full argument about why these are void.
//
// COMMAND-THREAD ONLY. The UI thread is the sole drainer of the command ring, so the std::function
// indirection below costs nothing that matters. That is not true of the producer path.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine_pure.h"
#include "engine_types.h"
#include "event_payloads.h"
#include "event_ring.h"

namespace daw::engine {

struct PlacementCommandDeps {
  const std::function<bool(uint32_t, const std::function<bool(std::vector<daw::ProjectPlacement>&)>&)>& applyPlacementEdit;
  const std::function<uint32_t(TrackRuntime*)>& bumpClipVersionFor;
  std::atomic<bool>& clipDirty;
  const std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>& historyAppend;
  std::atomic<uint32_t>& nextClipId;
  std::atomic<uint32_t>& nextPlacementId;
  const std::function<void(uint32_t, TrackStoreState, TrackStoreState)>& pushStructuralUndo;
  const std::function<void(EngineUndoEntry)>& pushUndo;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>& rebuildAudioRender;
  const std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)>& rebuildFlatAndPublish;
  const std::function<void()>& recomputeSongEnd;
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>& requireMatchingClipVersion;
  const std::function<TrackStoreState(const TrackRuntime&)>& snapshotTrackStore;
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
};

void handleForkSwapPlacementClip(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetPlacementEditScope(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRevertPlacementOverrides(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleMovePlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleAddPlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleResizePlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRemovePlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
