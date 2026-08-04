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
struct SamplerCommandDeps {
  // The read-back commands (RequestSamplerKit, RequestSamplerEnvelope) write their answer
  // straight into the published region, so the SHM state is a dependency like any other. It was
  // missed by the static scan of main's locals for a precise reason worth keeping: it was declared
  // as `struct UiShmState { ... } uiShm;`, one statement declaring a type AND defining an object,
  // so no pattern looking for a variable declaration could see it. The compiler found it.
  UiShmState& uiShm;
  TrackTable& trackTable;
  daw::TempoMapProvider& tempoProvider;

  const std::function<void(daw::UiCommandType, daw::UiSamplerRejectReason,
                           uint32_t, uint32_t, uint16_t)>& reportSamplerReject;
  const std::function<void(TrackRuntime&)>& refreshSamplerForTrack;
  const std::function<std::shared_ptr<const daw::SamplerRender>(
      const daw::SamplerState&, uint32_t, uint32_t)>& rebuildSamplerRender;
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t, bool,
                           std::optional<daw::EventId>, uint16_t, uint16_t)>& applyAddNote;
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

}  // namespace daw::engine
