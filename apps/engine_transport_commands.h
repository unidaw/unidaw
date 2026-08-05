#pragma once

// TRANSPORT: play, stop, position, tempo, the loop range, panic, and quit.
//
// Seven arms that between them decide WHEN the engine is playing and WHERE — the last coherent
// group of logic still living inline in handleUiEntry.
//
// GROUPED BY THE UNION OF THEIR CAPTURES. Taken one at a time these need 29 dependencies; taken
// together, 19. They are seven views onto one piece of state — the transport — so extracting them
// separately would have built seven interfaces onto it and cost more than it saved.
//
// Bodies moved VERBATIM and diffed against the arms they came from. See
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

#include "engine_track_table.h"
#include "engine_song_timing.h"
#include "engine_transport_state.h"
#include "engine_pure.h"
#include "engine_preview_queue.h"
#include "engine_state.h"
#include "engine_types.h"
#include "event_payloads.h"
#include "event_ring.h"

namespace daw::engine {

struct TransportCommandDeps {
  // The engine's state rather than 4 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  // Three members and a std::function became one: see apps/engine_preview_queue.h.
  std::unique_ptr<TrackRuntime>& masterTrack;
  std::atomic<bool>& panicPending;
  const uint64_t patternTicks;
  std::atomic<bool>& resetTimeline;
  std::condition_variable& restartCv;
  std::atomic<bool>& running;
  daw::TempoMapProvider& tempoProvider;
};

void handleSetLoopRange(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handlePanic(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetTempo(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleStop(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetPosition(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleQuit(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleTogglePlay(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
