// Bodies for apps/engine_rowops_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_rowops_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleSetRowOps(RowopsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  const auto& applySetRowOps = deps.applySetRowOps;
  const auto& emitClipReject = deps.emitClipReject;
  {
  daw::UiSetRowOpsPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  daw::RowOpEdit edit;
  edit.mask = p.mask;
  edit.retrigger = p.retrigger;
  edit.probability = p.probability;
  edit.retrigRamp = p.retrigRamp;
  edit.trigCondition = p.trigCondition;
  edit.sound = p.sound;
  edit.soundOffset = p.soundOffset;
  edit.delayNanoticks = p.delayNanoticks;
  // REASSEMBLED IN ONE PLACE. The id is 64 bits carried as two 32-bit halves — see the
  // payload's comment for why it is split rather than moved — and this is the only site
  // that puts them back together, so there is no second reading of the same value to
  // disagree with this one.
  const daw::EventId noteId =
      (static_cast<uint64_t>(p.noteIdHi) << 32) | static_cast<uint64_t>(p.noteIdLo);
  // A REFUSAL THE UI CAN SEE. rowops.rejected was a log line and nothing else, so from the
  // page the sequence was: the sidecar replies ok, the engine refuses into its own log, the
  // cell does not change, and the person is told nothing. That is the same silence the
  // stale-base clip edit had before ClipRejected existed — so this rides the same diff,
  // which already carries the refused commandType.
  daw::UiClipRejectReason rejectReason = daw::UiClipRejectReason::None;
  if (!applySetRowOps(p.trackId, p.clipId, noteId, edit, /*recordUndo=*/true,
                      rejectReason) &&
      rejectReason != daw::UiClipRejectReason::None) {
    emitClipReject(rejectReason, p.trackId, /*sentBase=*/0, /*currentBase=*/0,
                   daw::UiCommandType::SetRowOps);
  }
  return;
  }
}

}  // namespace daw::engine
