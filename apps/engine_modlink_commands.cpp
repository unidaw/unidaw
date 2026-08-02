// Bodies for apps/engine_modlink_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_modlink_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleAddModLink(ModlinkCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitModError = deps.emitModError;
  const auto& emitModSnapshot = deps.emitModSnapshot;
  const auto& historyAppend = deps.historyAppend;
  (void)tracks; (void)tracksMutex; (void)buildTrackSnapshot; (void)emitModError; (void)emitModSnapshot; (void)historyAppend;
  (void)entry; (void)header; (void)commandType;
  {
  daw::UiModLinkCommandPayload modPayload{};
  std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
  const auto commandType =
      static_cast<daw::UiCommandType>(modPayload.commandType);
  if (commandType != daw::UiCommandType::AddModLink &&
      commandType != daw::UiCommandType::RemoveModLink &&
      commandType != daw::UiCommandType::SetModLinkDepth) {
    return;
  }
  constexpr uint16_t kModErrTrackMissing = 1;
  constexpr uint16_t kModErrLinkMissing = 2;
  constexpr uint16_t kModErrInvalidKind = 3;
  constexpr uint16_t kModErrInvalidDevice = 4;
  constexpr uint16_t kModErrOrderViolation = 5;
  constexpr uint16_t kModErrLinkExists = 6;
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (modPayload.trackId < tracks.size()) {
      runtime = tracks[modPayload.trackId].get();
    }
  }
  if (!runtime) {
    emitModError(kModErrTrackMissing, modPayload.trackId, modPayload.linkId);
    return;
  }
  // A REMOVE NEEDS ONLY (track, link), AND A DEPTH CHANGE ONLY (track, link, depth). Both
  // used to fall through the ADD's validation below — kind decoding, findDevicePos on both
  // device ids, and the forward-order test — so a caller that knew a link's id still had to
  // send the devices it happens to connect. Unstated ids default to 0, so on a project whose
  // device ids start higher EVERY removal was refused as kModErrInvalidDevice while the
  // caller was told it succeeded, and the links piled up. It looked correct only because
  // rack.uniproj.json has a device 0, so the default resolved there.
  //
  // Reported by the frontend agent, who worked around it by looking each link up and sending
  // its devices. Validating what a command does not use is how a command acquires arguments
  // that have nothing to do with it.
  if (commandType == daw::UiCommandType::RemoveModLink ||
      commandType == daw::UiCommandType::SetModLinkDepth) {
    const bool removing = commandType == daw::UiCommandType::RemoveModLink;
    bool touched = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      auto& links = runtime->track.modRegistry.links;
      if (removing) {
        const auto before = links.size();
        links.erase(std::remove_if(links.begin(), links.end(),
                                   [&](const daw::ModLink& link) {
                                     return link.linkId == modPayload.linkId;
                                   }),
                    links.end());
        touched = links.size() != before;
      } else {
        // IN PLACE, so the id, the uid16 and the source/target survive. Remove+add was the
        // only way to change a depth, and it changed the id, dropped the uid16 (which
        // silently disables the modulation) and was not atomic — which put a depth SLIDER
        // out of reach, since a continuous gesture would tear the link down and rebuild it
        // every frame. That was a UI limitation caused by the opcode set.
        for (auto& link : links) {
          if (link.linkId != modPayload.linkId) {
            continue;
          }
          link.depth = modPayload.depth;
          link.bias = modPayload.bias;
          link.enabled = ((modPayload.flags >> 10) & 0x1u) != 0;
          touched = true;
          break;
        }
      }
    }
    if (!touched) {
      emitModError(kModErrLinkMissing, modPayload.trackId, modPayload.linkId);
      return;
    }
    std::shared_ptr<const TrackStateSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snapshot = buildTrackSnapshot(runtime->track);
    }
    std::atomic_store_explicit(&runtime->trackSnapshot, snapshot,
                               std::memory_order_release);
    emitModSnapshot(*runtime);
    DAW_EVENT(removing ? "modlink.removed" : "modlink.depth_set")
        .field("track", modPayload.trackId)
        .field("link", modPayload.linkId)
        .field("depth", static_cast<double>(modPayload.depth))
        .field("bias", static_cast<double>(modPayload.bias));
    historyAppend(daw::uiCommandTypeName(commandType), "received",
                  modPayload.trackId, 0, "");
    return;
  }

  auto decodeSourceKind = [&](uint16_t flags) -> std::optional<daw::ModSourceKind> {
    const uint8_t raw = static_cast<uint8_t>(flags & 0x0Fu);
    if (raw > static_cast<uint8_t>(daw::ModSourceKind::PatcherNodeOutput)) {
      return std::nullopt;
    }
    return static_cast<daw::ModSourceKind>(raw);
  };
  auto decodeTargetKind = [&](uint16_t flags) -> std::optional<daw::ModTargetKind> {
    const uint8_t raw = static_cast<uint8_t>((flags >> 4) & 0x0Fu);
    if (raw > static_cast<uint8_t>(daw::ModTargetKind::PatcherMacro)) {
      return std::nullopt;
    }
    return static_cast<daw::ModTargetKind>(raw);
  };
  auto decodeRate = [&](uint16_t flags) -> std::optional<daw::ModRate> {
    const uint8_t raw = static_cast<uint8_t>((flags >> 8) & 0x03u);
    if (raw > static_cast<uint8_t>(daw::ModRate::SampleRate)) {
      return std::nullopt;
    }
    return static_cast<daw::ModRate>(raw);
  };
  const bool enabled = ((modPayload.flags >> 10) & 0x1u) != 0;
  auto sourceKind = decodeSourceKind(modPayload.flags);
  auto targetKind = decodeTargetKind(modPayload.flags);
  auto rate = decodeRate(modPayload.flags);
  if (!sourceKind || !targetKind || !rate) {
    emitModError(kModErrInvalidKind, modPayload.trackId, modPayload.linkId);
    return;
  }
  std::vector<daw::Device> devices;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    devices = runtime->track.chain.devices;
  }
  auto findDevicePos = [&](uint32_t deviceId) -> std::optional<size_t> {
    for (size_t i = 0; i < devices.size(); ++i) {
      if (devices[i].id == deviceId) {
        return i;
      }
    }
    return std::nullopt;
  };
  auto sourcePos = findDevicePos(modPayload.sourceDeviceId);
  auto targetPos = findDevicePos(modPayload.targetDeviceId);
  if (!sourcePos || !targetPos) {
    emitModError(kModErrInvalidDevice, modPayload.trackId, modPayload.linkId);
    return;
  }
  // Modulation flows FORWARD, so a device later in the chain must not modulate an
  // earlier one — by the time its value exists, the earlier device's audio has
  // already gone past. SAME device is fine and is in fact the common case now that
  // patchers are per-device: an LFO in device N's own graph driving device N's
  // cutoff is the ordinary thing to want.
  //
  // This used to reject same-device links (>= rather than >), which meant the
  // engine ACCEPTED from a file what it REFUSED from the UI — the loader installs
  // mod links without this check. presets/projects/rack.uniproj.json ships exactly
  // such a link, so the rack demo's modulation worked on load and could never be
  // recreated by hand. Found by daw_lint (M2.20) on its first run over the presets.
  if (*sourcePos > *targetPos) {
    emitModError(kModErrOrderViolation, modPayload.trackId, modPayload.linkId);
    return;
  }
  // ADD ONLY from here — remove and depth returned above.
  bool updated = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    {
      auto& links = runtime->track.modRegistry.links;
      if (modPayload.linkId == daw::kModLinkIdAuto) {
        uint32_t nextId = 1;
        for (const auto& link : links) {
          nextId = std::max(nextId, link.linkId + 1);
        }
        modPayload.linkId = nextId;
        // SAY WHICH ID. The caller sent the AUTO sentinel, so until this event existed the
        // only thing it could report was the sentinel itself — and a caller that then passed
        // 4294967295 to RemoveModLink matched nothing. Same shape as addPatcherNode's
        // UINT32_MAX-on-failure being reported as a new node id.
        DAW_EVENT("modlink.added")
            .field("track", modPayload.trackId)
            .field("link", nextId)
            .field("auto", true);
      } else {
        const bool exists =
            std::any_of(links.begin(),
                        links.end(),
                        [&](const daw::ModLink& link) {
                          return link.linkId == modPayload.linkId;
                        });
        if (exists) {
          emitModError(kModErrLinkExists, modPayload.trackId,
                       modPayload.linkId);
          return;
        }
      }
      daw::ModLink link{};
      link.linkId = modPayload.linkId;
      link.source.deviceId = modPayload.sourceDeviceId;
      link.source.sourceId = modPayload.sourceId;
      link.source.kind = *sourceKind;
      link.target.deviceId = modPayload.targetDeviceId;
      link.target.targetId = modPayload.targetId;
      link.target.kind = *targetKind;
      link.depth = modPayload.depth;
      link.bias = modPayload.bias;
      link.rate = *rate;
      link.enabled = enabled;
      links.push_back(link);
      updated = true;
    }
  }
  if (updated) {
    std::shared_ptr<const TrackStateSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snapshot = buildTrackSnapshot(runtime->track);
    }
    std::atomic_store_explicit(&runtime->trackSnapshot,
                               snapshot,
                               std::memory_order_release);
    emitModSnapshot(*runtime);
  }
  return;
  }
}

void handleSetModLinkUid16(ModlinkCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitModError = deps.emitModError;
  const auto& emitModSnapshot = deps.emitModSnapshot;
  const auto& historyAppend = deps.historyAppend;
  (void)tracks; (void)tracksMutex; (void)buildTrackSnapshot; (void)emitModError; (void)emitModSnapshot; (void)historyAppend;
  (void)entry; (void)header; (void)commandType;
  {
  daw::UiModLinkUid16Payload modPayload{};
  std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
  if (modPayload.commandType !=
      static_cast<uint16_t>(daw::UiCommandType::SetModLinkUid16)) {
    return;
  }
  constexpr uint16_t kModErrTrackMissing = 1;
  constexpr uint16_t kModErrLinkMissing = 2;
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (modPayload.trackId < tracks.size()) {
      runtime = tracks[modPayload.trackId].get();
    }
  }
  if (!runtime) {
    emitModError(kModErrTrackMissing, modPayload.trackId, modPayload.linkId);
    return;
  }
  bool updated = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& link : runtime->track.modRegistry.links) {
      if (link.linkId != modPayload.linkId) {
        continue;
      }
      std::memcpy(link.target.uid16,
                  modPayload.uid16,
                  sizeof(link.target.uid16));
      updated = true;
      break;
    }
  }
  if (updated) {
    std::shared_ptr<const TrackStateSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snapshot = buildTrackSnapshot(runtime->track);
    }
    std::atomic_store_explicit(&runtime->trackSnapshot,
                               snapshot,
                               std::memory_order_release);
    emitModSnapshot(*runtime);
  } else {
    emitModError(kModErrLinkMissing, modPayload.trackId, modPayload.linkId);
  }
  return;
  }
}

void handleSetModSourceValue(ModlinkCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitModError = deps.emitModError;
  const auto& emitModSnapshot = deps.emitModSnapshot;
  const auto& historyAppend = deps.historyAppend;
  (void)tracks; (void)tracksMutex; (void)buildTrackSnapshot; (void)emitModError; (void)emitModSnapshot; (void)historyAppend;
  (void)entry; (void)header; (void)commandType;
  {
  daw::UiModSourceValuePayload modPayload{};
  std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
  if (modPayload.commandType !=
      static_cast<uint16_t>(daw::UiCommandType::SetModSourceValue)) {
    return;
  }
  constexpr uint16_t kModErrTrackMissing = 1;
  constexpr uint16_t kModErrInvalidKind = 3;
  constexpr uint16_t kModErrInvalidDevice = 4;
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (modPayload.trackId < tracks.size()) {
      runtime = tracks[modPayload.trackId].get();
    }
  }
  if (!runtime) {
    emitModError(kModErrTrackMissing, modPayload.trackId, 0);
    return;
  }
  const uint8_t rawKind = static_cast<uint8_t>(modPayload.flags & 0x0Fu);
  if (rawKind > static_cast<uint8_t>(daw::ModSourceKind::PatcherNodeOutput)) {
    emitModError(kModErrInvalidKind, modPayload.trackId, 0);
    return;
  }
  const auto kind = static_cast<daw::ModSourceKind>(rawKind);
  bool deviceFound = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (const auto& device : runtime->track.chain.devices) {
      if (device.id == modPayload.sourceDeviceId) {
        deviceFound = true;
        break;
      }
    }
  }
  if (!deviceFound) {
    emitModError(kModErrInvalidDevice, modPayload.trackId, 0);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(runtime->modSourcesMutex);
    auto& sources = runtime->modSources;
    bool updated = false;
    for (auto& source : sources) {
      if (source.ref.deviceId == modPayload.sourceDeviceId &&
          source.ref.sourceId == modPayload.sourceId &&
          source.ref.kind == kind) {
        source.value = modPayload.value;
        updated = true;
        break;
      }
    }
    if (!updated) {
      daw::ModSourceState state{};
      state.ref.deviceId = modPayload.sourceDeviceId;
      state.ref.sourceId = modPayload.sourceId;
      state.ref.kind = kind;
      state.value = modPayload.value;
      sources.push_back(state);
    }
  }
  return;
  }
}

}  // namespace daw::engine
