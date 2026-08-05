#pragma once

// ARRANGEMENT TIME: the meter, and inserting or removing arrangement time.
//
// Two opcodes — SetTimeSignature and InsertRemoveTime — sharing one payload struct and, until this
// commit, one 369-line block sitting inline in handleUiEntry. It was the largest single piece of
// logic left in that function, and the reason the function is 1,746 lines rather than a dispatcher.
//
// EXTRACTED BY CAPTURES, NOT BY SIZE, which is the rule this repo arrived at the hard way. 369
// lines cost NINETEEN dependencies. The enclosing HandleUiEntryDeps has 72, so this block reaches
// for barely a quarter of what its neighbours collectively need — that ratio, not the line count,
// is what made it the right piece to move first.
//
// The body moved VERBATIM: the same statements in the same order behind identically-named
// references, proven by diffing the moved body against the deleted one line for line. See
// apps/engine_sampler_commands.h for the full argument about why these handlers are void and what
// that preserves.
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

#include "engine_arrange_rail.h"
#include "engine_song_timing.h"
#include "engine_harmony_timeline.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/markers.h"
#include "apps/time_signature_map.h"
#include "apps/engine_state.h"

namespace daw::engine {

struct ArrangeTimeCommandDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  std::atomic<uint32_t>& automationVersion;
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>& buildTrackSnapshot;
  const std::function<uint32_t(TrackRuntime*)>& bumpClipVersionFor;
  std::atomic<bool>& clipDirty;
  HarmonyTimeline& harmonyTimeline;
  const std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>& historyAppend;
  const std::function<void(EngineUndoEntry)>& pushUndo;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>& rebuildAudioRender;
  const std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)>& rebuildFlatAndPublish;
  const std::function<void()>& recomputeSongEnd;
  const std::function<SongStoreState()>& snapshotSongStore;
  const std::function<std::vector<TrackRuntime*>()>& snapshotTracks;
  daw::TempoMapProvider& tempoProvider;
};

// Both opcodes arrive on UiArrangeTimeCommandPayload; the caller has already checked the size and
// that commandType is one of the two. commandType selects which of the two this is, exactly as it
// did when this was an if/else inside the dispatch chain.
void handleArrangeTime(ArrangeTimeCommandDeps& deps,
            const daw::EventEntry& entry,
            daw::UiCommandType commandType);

}  // namespace daw::engine
