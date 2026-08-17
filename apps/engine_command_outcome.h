#pragma once

#include <cstdint>
#include <functional>

#include "apps/event_payloads.h"
#include "apps/shared_memory.h"

namespace daw::engine {

// One signature from the version guards through the terminal handlers to the SHM publisher.
// `currentVersionValid` is false for UnknownTrack; only a valid StaleBase version may be retried.
using CommandOutcomePublisher = std::function<void(
    daw::UiCommandType,
    daw::UiCommandOutcomeKind,
    daw::UiCommandOutcomeReason,
    uint32_t,
    uint32_t,
    bool,
    uint32_t)>;

}  // namespace daw::engine
