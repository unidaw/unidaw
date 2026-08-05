#pragma once

// THE PER-TRACK PROPERTY SETTERS
//
// Seven arms that each set one property of one track: mixer, lines-per-beat, lane
// quantize, harmony-quantize, note overlap, collapsed, sound-addressed. 240 lines
// against six names.
//
// Extracted from handleUiEntry, a 5,604-line lambda inside main() that is a flat sequence of
// independent dispatch blocks. Bodies moved verbatim; see apps/engine_sampler_commands.h for the
// full argument about why these are void and what that preserves.
//
// COMMAND-THREAD ONLY. The UI thread is the sole drainer of the command ring, so the std::function
// indirection below costs nothing that matters. That is not true of the producer path.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "engine_track_table.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct TrackpropsCommandDeps {
  TrackTable& trackTable;
  std::unique_ptr<TrackRuntime>& masterTrack;
  std::atomic<uint32_t>& quantizeVersion;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
};

void handleSetTrackMixer(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetTrackHarmonyQuantize(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetTrackSoundAddressed(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetTrackCollapsed(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetTrackLinesPerBeat(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetTrackAllowNoteOverlap(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetLaneQuantize(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
