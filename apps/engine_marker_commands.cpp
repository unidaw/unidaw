// Bodies for apps/engine_marker_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_marker_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleAddMarker(MarkerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& markerList = deps.markerList;
  auto& arrangeMutex = deps.arrangeMutex;
  auto& arrangeVersion = deps.arrangeVersion;
  const auto& historyAppend = deps.historyAppend;
  {
  daw::UiMarkerCommandPayload mp{};
  std::memcpy(&mp, entry.payload, sizeof(mp));
  if (static_cast<daw::UiCommandType>(mp.commandType) != commandType) {
    return;
  }
  const std::string name(mp.name, strnlen(mp.name, sizeof(mp.name)));
  const uint64_t tick = (static_cast<uint64_t>(mp.nanotickHi) << 32) | mp.nanotickLo;
  bool ok = false;
  const char* what = "";
  const char* reason = "no_such_marker";
  uint32_t markerId = mp.markerId;
  {
    std::lock_guard<std::mutex> alock(arrangeMutex);
    if (commandType == daw::UiCommandType::AddMarker) {
      daw::Marker m;
      m.id = mp.markerId;  // 0 = let the list assign from its watermark
      m.nanotick = tick;
      m.name = name.empty() ? "Marker" : name;
      m.colorRgb = mp.colorRgb;
      // The ASSIGNED id comes back, so a caller that sent 0 learns which marker it made.
      // Reporting the sentinel instead is a mistake this codebase has made twice already.
      markerId = markerList.add(std::move(m));
      ok = markerId != 0;
      reason = "id_exists";
      what = "added";
    } else if (commandType == daw::UiCommandType::RemoveMarker) {
      ok = markerList.remove(mp.markerId);
      what = "removed";
    } else if (commandType == daw::UiCommandType::RenameMarker) {
      if (name.empty()) {
        reason = "empty_name";  // a marker with no name is a flag with nothing to read
      } else {
        ok = markerList.rename(mp.markerId, name);
      }
      what = "renamed";
    } else if (commandType == daw::UiCommandType::SetMarkerColor) {
      // NO VALIDATION. Every 24-bit value is a legal colour, including 0 — so unlike the
      // rename above there is no "empty" case to refuse, and the only way this fails is an
      // id that is not there. That is also the reason this is not a flag on RenameMarker:
      // black being legal means an omitted colour could not be told from a chosen one.
      ok = markerList.setColor(mp.markerId, mp.colorRgb);
      what = "recoloured";
    } else {
      ok = markerList.moveTo(mp.markerId, tick);
      what = "moved";
    }
  }
  if (!ok) {
    DAW_EVENT("marker.rejected")
        .field("op", daw::uiCommandTypeName(commandType))
        .field("marker", mp.markerId)
        .field("reason", reason);
    historyAppend(daw::uiCommandTypeName(commandType),
                  (std::string("rejected:") + reason).c_str(), 0xFFFFFFFFu, 0, "");
    return;
  }
  arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
  DAW_EVENT("marker.changed")
      .field("op", daw::uiCommandTypeName(commandType))
      .field("marker", markerId)
      .field("nanotick", tick)
      .field("what", what);
  historyAppend(daw::uiCommandTypeName(commandType), "received", 0xFFFFFFFFu, 0, "");
  return;
  }
}

}  // namespace daw::engine
