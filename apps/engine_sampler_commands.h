#pragma once

// THE SAMPLER'S UI COMMANDS — twelve opcodes that used to live inside handleUiEntry.
//
// handleUiEntry is a 5,604-line lambda inside main(), which is 30% of main()'s body and the
// single largest thing in the engine. It is not one function in any useful sense: it is a flat
// sequence of independent dispatch blocks, each testing a payload size and a command type and
// each ending in `return;`. This file takes the twelve that belong to the sampler.
//
// WHY THIS FAMILY FIRST: it is both the largest (1,463 lines) and the least entangled. It reaches
// seven names from main's scope; the transport family reaches thirty-five for a third of the
// lines. Taking the biggest, loosest one first is the opposite of what "start with something
// small" would suggest, and it is right here because size and coupling happen to be inversely
// ordered.
//
// EVERY HANDLER IS COMMAND-THREAD. The UI thread is the only drainer of the command ring, so
// nothing here needs to be lock-free or allocation-free — the std::function indirection below
// costs nothing that matters. That is emphatically NOT true of the producer path, and this
// distinction is the reason the two are being extracted with different rules.
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "engine_track_table.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/sampler_engine.h"
#include "apps/sampler_state.h"
#include "apps/time_base.h"

namespace daw::engine {

// What the sampler commands need from main(). Explicit, and small enough to read: the whole
// point of the extraction is that this list EXISTS rather than being implied by a [&] capture of
// two hundred and ninety-seven names.
//
// The four callables stay as they were written — passing them keeps the handlers a pure move.
// Turning them into methods on some engine object is a later step and a separate argument.
// WHAT REBUILDING A KIT NEEDS, and nothing more. Split out from SamplerCommandDeps for one
// concrete reason: main() resolves a sample path while it is still LOADING the project, ~700
// lines before the command deps can exist (those need applyAddNote, which needs the clip store).
// A struct of references cannot be built before its members do, so the two operations that are
// needed early get their own struct built early, and SamplerCommandDeps holds a reference to it.
// This is the same remedy TrackSetupDeps/TrackLifecycleDeps uses, and it is the documented answer
// to main()'s linear declaration order.
struct SamplerRefreshDeps {
  const daw::HostConfig& engineConfig;
  std::atomic<uint32_t>& samplerKitVersion;
  std::function<std::shared_ptr<const daw::SamplerRender>(
      const daw::SamplerState&, uint32_t, uint32_t)> rebuildSamplerRender;
};

struct SamplerCommandDeps {
  // The read-back commands (RequestSamplerKit, RequestSamplerEnvelope) write their answer
  // straight into the published region, so the SHM state is a dependency like any other. It was
  // missed by the static scan of main's locals for a precise reason worth keeping: it was declared
  // as `struct UiShmState { ... } uiShm;`, one statement declaring a type AND defining an object,
  // so no pattern looking for a variable declaration could see it. The compiler found it.
  UiShmState& uiShm;
  TrackTable& trackTable;
  daw::TempoMapProvider& tempoProvider;
  // refreshSamplerForTrack USED TO BE A MEMBER here — a std::function main() built and handed
  // over. It is a function in this file now, and the command handlers below reach it directly, so
  // the indirection is gone rather than relocated. What it needs arrives as SamplerRefreshDeps.
  SamplerRefreshDeps& samplerRefreshDeps;

  std::function<void(daw::UiCommandType, daw::UiSamplerRejectReason,
                           uint32_t, uint32_t, uint16_t)> reportSamplerReject;
  std::function<std::shared_ptr<const daw::SamplerRender>(
      const daw::SamplerState&, uint32_t, uint32_t)> rebuildSamplerRender;
  std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t, bool,
                           std::optional<daw::EventId>, uint16_t, uint16_t)> applyAddNote;
};

void handleSamplerEmitRows(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSlice(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleRequestSamplerEnvelope(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleRequestSamplerKit(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSetSlot(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSetDevice(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSetFilter(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSetVintage(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSetLfo(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerSetEnvelope(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSamplerLoad(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

// TAKES loadedProjectDir DIRECTLY RATHER THAN A DEPS STRUCT, and not for brevity: rebuildSampler-
// Render calls this, and SamplerRefreshDeps holds rebuildSamplerRender, so a struct parameter here
// would need the struct to exist before the lambda that the struct refers to. A pure function of
// (project directory, stored path) has no such cycle — and that is what this always was.
//
// Where does this sample actually live? A project stores sample paths RELATIVE to itself, so the
// same project opened from a different directory has to resolve them again. Falls back to the
// sibling audio/ directory and to the configured search path before giving up.
std::string resolveSourcePath(const std::string& loadedProjectDir,
                              const std::string& sourcePath);

// Rebuild the render for whatever kit this track now has and publish the new version. Every
// sampler edit ends here; it is the one place that turns edited SamplerState into something the
// producer can play.
void refreshSamplerForTrack(SamplerRefreshDeps& deps, TrackRuntime& rt);

}  // namespace daw::engine
