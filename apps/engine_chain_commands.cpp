// Bodies for apps/engine_chain_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_chain_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleAddDevice(ChainCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  auto& masterTrack = deps.masterTrack;
  auto& pluginCache = deps.pluginCache;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitChainError = deps.emitChainError;
  const auto& emitChainSnapshot = deps.emitChainSnapshot;
  const auto& rebuildHostForChain = deps.rebuildHostForChain;
  const auto& reconcileMasterHost = deps.reconcileMasterHost;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  {
  daw::UiChainCommandPayload chainPayload{};
  std::memcpy(&chainPayload, entry.payload, sizeof(chainPayload));
  const auto commandType =
      static_cast<daw::UiCommandType>(chainPayload.commandType);
  TrackRuntime* runtime = nullptr;
  if (chainPayload.trackId == daw::kMasterTrackId) {
    // The master is addressed by its stable id, not a slot; it lives outside the
    // tracks vector. Its chain accepts the same device edits as any track.
    runtime = masterTrack.get();
  } else runtime = daw::engine::trackAt(tracks, tracksMutex, chainPayload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: Chain command failed - track "
              << chainPayload.trackId << " not found" << std::endl;
    return;
  }
  bool chainChanged = false;
  bool emitError = false;
  uint16_t errorCode = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    if (commandType == daw::UiCommandType::AddDevice) {
      daw::Device device;
      // ZERO MEANS "PICK ONE", not "call it zero". Everywhere else on this wire a deviceId of
      // 0 means unspecified — the sampler commands all read it as "the first sampler on the
      // track, whichever that is" — and callers send 0 when they do not care. This took it
      // literally, so `add-device --kind sampler` on a fresh track created a device whose id
      // WAS 0, which is the same value the engine uses for "this track has no sampler".
      // Nine guards then skipped it and the instrument was never sent a note.
      device.id = chainPayload.deviceId != 0 ? chainPayload.deviceId : daw::kDeviceIdAuto;
      device.kind = static_cast<daw::DeviceKind>(chainPayload.deviceKind);
      device.patcherNodeId = chainPayload.patcherNodeId;
      // THE WIRE IS UNCHANGED, and the translation happens here at the boundary. AddDevice still
      // carries a hostSlotIndex, and kHostSlotIndexDirect on it still means "load by path" — the
      // same sentence the file format used before load_mode existed. Translating it into the
      // authored field at the point of entry keeps load_mode REACHABLE (persisted_field_reach
      // caught that the split had orphaned it: a field the format remembers and nothing can write)
      // without an opcode or SHM change.
      device.loadMode = chainPayload.hostSlotIndex == daw::kHostSlotIndexDirect
                            ? daw::VstLoadMode::ByPath
                            : daw::VstLoadMode::ByReference;
      device.hostSlotIndex = chainPayload.hostSlotIndex;
      // Record the DURABLE plugin identity too, not just the volatile scan index.
      // hostSlotIndex names a different plugin the moment anything is installed or
      // removed, so a project saved with only the index reloads the wrong plugin
      // silently. vstRef is what the loader actually keys on; fill it from the
      // cache entry the slot resolves to, so a device added through AddDevice is
      // as durable as one from a loaded project.
      if ((device.kind == daw::DeviceKind::VstInstrument ||
           device.kind == daw::DeviceKind::VstEffect) &&
          device.hostSlotIndex < pluginCache.entries.size()) {
        const auto& entry = pluginCache.entries[device.hostSlotIndex];
        device.vstRef.vendor = entry.vendor;
        device.vstRef.name = entry.name;
        device.vstRef.path = entry.path;
        device.vstRef.uid16 = entry.pluginUid16;
      }
      // A NEW SAMPLER ARRIVES ABLE TO MAKE A SOUND. It has one mod set with an amp
      // envelope whose attack is INSTANT, because the first thing anyone drops on a sampler
      // is a drum and a 10 ms attack on a kick is a defect you have to go looking for. It
      // has no slots yet — sampler-load mints those — so it is silent until a sample is
      // loaded, which is honest rather than surprising.
      if (device.kind == daw::DeviceKind::Sampler) {
        device.hasSampler = true;
        device.sampler = daw::SamplerState{};
        device.sampler.modSets.push_back(daw::defaultModSet(1));
        device.sampler.nextModSetId = 2;
      }
      device.bypass = chainPayload.bypass != 0;
      device.capabilityMask = daw::capabilityMaskForKind(device.kind);
      chainChanged = daw::addDevice(runtime->track.chain,
                                    device,
                                    chainPayload.insertIndex);
      if (!chainChanged) {
        emitError = true;
        errorCode = 1;
      }
    } else if (commandType == daw::UiCommandType::RemoveDevice) {
      chainChanged = daw::removeDeviceById(runtime->track.chain,
                                           chainPayload.deviceId);
      if (!chainChanged) {
        emitError = true;
        errorCode = 2;
      }
    } else if (commandType == daw::UiCommandType::MoveDevice) {
      chainChanged = daw::moveDeviceById(runtime->track.chain,
                                         chainPayload.deviceId,
                                         chainPayload.insertIndex);
      if (!chainChanged) {
        emitError = true;
        errorCode = 3;
      }
    } else if (commandType == daw::UiCommandType::UpdateDevice) {
      const uint16_t flags = chainPayload.flags;
      if (flags & 0x1u) {
        chainChanged |= daw::setDeviceBypass(runtime->track.chain,
                                             chainPayload.deviceId,
                                             chainPayload.bypass != 0);
      }
      if (flags & 0x2u) {
        chainChanged |= daw::setDevicePatcherNodeId(runtime->track.chain,
                                                    chainPayload.deviceId,
                                                    chainPayload.patcherNodeId);
      }
      if (flags & 0x4u) {
        chainChanged |= daw::setDeviceHostSlotIndex(runtime->track.chain,
                                                    chainPayload.deviceId,
                                                    chainPayload.hostSlotIndex);
      }
      if (!chainChanged) {
        emitError = true;
        errorCode = 4;
      }
    }
  }
  if (chainChanged) {
    std::shared_ptr<const TrackStateSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // ADD, REMOVE AND MOVE ALL CHANGE WHETHER A TRACK HAS A SAMPLER, and none of them said
      // so. refreshSamplerForTrack's own comment claims it is "called from EVERY site that
      // changes a chain, so 'did you remember to rebuild the sampler' is not a question
      // anyone has to answer twice" — and this, the site that adds and removes devices, was
      // not one of them. The comment was the assertion, and comments do not run.
      //
      // A sampler added through AddDevice therefore had no snapshot: not installed on the
      // audio thread, and its kit read-back answering found:false, which is the same answer
      // as "there is no sampler on that device". That is exactly the interval — created but
      // not yet loaded — when a UI most wants to say "here it is, put something in it", and
      // it could say nothing. Reported by the web-UI agent from the outside, as the only
      // symptom visible from there.
      //
      // Removing a sampler matters just as much in the other direction: without this the
      // snapshot outlives the device and the track keeps playing an instrument that is no
      // longer in its chain.
      refreshSamplerForTrack(*runtime);
      snapshot = buildTrackSnapshot(runtime->track);
    }
    std::atomic_store_explicit(&runtime->trackSnapshot,
                               snapshot,
                               std::memory_order_release);
    // Reconcile the host. The master runs its own lifecycle (it is not in the tracks
    // vector); a patcher/mod-only master resolves to no plugins and launches nothing,
    // while a VST effect on the master brings its host up for the 4b sum-processing path.
    if (chainPayload.trackId == daw::kMasterTrackId) {
      reconcileMasterHost();
    } else {
      rebuildHostForChain(*runtime);
    }
    emitChainSnapshot(*runtime);
  } else if (emitError) {
    emitChainError(errorCode,
                   chainPayload.trackId,
                   chainPayload.deviceId,
                   chainPayload.deviceKind,
                   chainPayload.insertIndex);
  }
  return;
  }
}

}  // namespace daw::engine
