// Bodies for apps/engine_patcher_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_patcher_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include "apps/patcher_preset_library.h"
#include <filesystem>

namespace daw::engine {

// Decode a SetPatcherNodeConfig payload's config block and apply it to ONE graph state.
//
// Extracted so the shared-pool path and the per-device path share a single decoder. They had to:
// the block is an explicit little-endian layout per node type rather than a struct memcpy, and
// two copies of a hand-written layout is the same "two facts about one thing" that makes a
// mirror go stale — the second copy would be correct on the day it was written and wrong the
// first time a field moved.
//
// Returns false and sets `failure` when the node does not exist or the type carries no config.
bool applyNodeConfigTo(daw::PatcherGraphState& state,
                       const daw::UiPatcherNodeConfigPayload& p,
                       const char** failure) {
  const uint8_t* cfg = p.config;
  auto rdU16 = [&](int i) -> uint32_t {
    return static_cast<uint32_t>(cfg[i]) | (static_cast<uint32_t>(cfg[i + 1]) << 8);
  };
  auto rdU32 = [&](int i) -> uint32_t {
    return static_cast<uint32_t>(cfg[i]) |
           (static_cast<uint32_t>(cfg[i + 1]) << 8) |
           (static_cast<uint32_t>(cfg[i + 2]) << 16) |
           (static_cast<uint32_t>(cfg[i + 3]) << 24);
  };
  const auto type = static_cast<daw::PatcherNodeType>(p.configType);
  bool updated = false;
  switch (type) {
    case daw::PatcherNodeType::Euclidean: {
      daw::PatcherEuclideanConfig c{};
      c.steps = rdU16(0);
      c.hits = rdU16(2);
      c.offset = rdU16(4);
      c.degree = cfg[6];
      c.octave_offset = static_cast<int8_t>(cfg[7]);
      c.velocity = cfg[8];
      c.base_octave = cfg[9];
      c.duration_ticks = rdU32(12);
      updated = daw::setEuclideanConfig(state, p.nodeId, c);
      break;
    }
    case daw::PatcherNodeType::RandomDegree: {
      daw::PatcherRandomDegreeConfig c{};
      c.degree = cfg[0];
      c.velocity = cfg[1];
      c.duration_ticks = rdU32(4);
      updated = daw::setRandomDegreeConfig(state, p.nodeId, c);
      break;
    }
    case daw::PatcherNodeType::SliceSelect: {
      daw::PatcherSliceSelectConfig c{};
      c.base = static_cast<uint16_t>(rdU16(0));
      c.count = static_cast<uint16_t>(rdU16(2));
      updated = daw::setSliceSelectConfig(state, p.nodeId, c);
      break;
    }
    case daw::PatcherNodeType::Lfo: {
      daw::PatcherLfoConfig c{};
      c.frequency_hz = static_cast<int32_t>(rdU32(0)) / 1000.0f;
      c.depth = static_cast<int32_t>(rdU32(4)) / 1000.0f;
      c.bias = static_cast<int32_t>(rdU32(8)) / 1000.0f;
      c.phase_offset = static_cast<int32_t>(rdU32(12)) / 1000.0f;
      updated = daw::setLfoConfig(state, p.nodeId, c);
      break;
    }
    default:
      if (failure) {
        *failure = "invalid_type";
      }
      return false;
  }
  if (!updated && failure) {
    *failure = "invalid_node";
  }
  return updated;
}


void handleAddPatcherNode(PatcherCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& patcherGraphState = deps.patcherGraph.patcherGraphState;
  auto& patcherPoolEdited = deps.patcherGraph.patcherPoolEdited;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitPatcherGraphDelta = deps.emitPatcherGraphDelta;
  const auto& emitPatcherGraphError = deps.emitPatcherGraphError;
  const auto& reassemblePatcherFromDevices = deps.reassemblePatcherFromDevices;
  const auto& updatePatcherGraphSnapshot = deps.updatePatcherGraphSnapshot;
  {
  daw::UiPatcherGraphCommandPayload probe{};
  std::memcpy(&probe, entry.payload, sizeof(probe));
  if ((probe.flags & daw::kUiPatcherFlagHasDeviceId) != 0) {
    const uint32_t deviceId =
        static_cast<uint32_t>(probe.flags & daw::kUiPatcherDeviceIdMask);
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, probe.trackId);
    auto refuse = [&](const char* why) {
      DAW_EVENT("patcher_device_edit.rejected")
          .field("track", probe.trackId)
          .field("device", deviceId)
          .field("op", daw::uiCommandTypeName(commandType))
          .field("reason", why);
    };
    if (!runtime) {
      refuse("no_such_track");
      return;
    }
    bool applied = false;
    uint32_t newNodeId = 0;
    const char* failure = nullptr;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      daw::Device* device = nullptr;
      for (auto& d : runtime->track.chain.devices) {
        if (d.id == deviceId) {
          device = &d;
          break;
        }
      }
      if (!device) {
        failure = "no_such_device";
      } else {
        // Scratch state around THIS device's authored graph. nextNodeId comes from the
        // graph itself so a new node cannot collide with one already in it.
        daw::PatcherGraphState scratch;
        scratch.graph = device->patcher;
        uint32_t next = 0;
        for (const auto& n : scratch.graph.nodes) {
          next = std::max(next, n.id + 1);
        }
        scratch.nextNodeId = next;
        if (commandType == daw::UiCommandType::AddPatcherNode) {
          if (probe.nodeType >
              static_cast<uint32_t>(daw::kPatcherNodeTypeMax)) {
            failure = "invalid_node_type";
          } else {
            newNodeId = daw::addPatcherNode(
                scratch, static_cast<daw::PatcherNodeType>(probe.nodeType));
            // addPatcherNode returns UINT32_MAX when the graph will not BUILD with the new
            // node and rolls it back. Treating that as success reported an edit that had been
            // refused — and the report even carried 4294967295 as the new node id, which is
            // the sentinel announcing itself.
            applied = newNodeId != std::numeric_limits<uint32_t>::max();
            if (!applied) {
              failure = "graph_would_not_build";
            }
          }
        } else if (commandType == daw::UiCommandType::RemovePatcherNode) {
          applied = daw::removePatcherNode(scratch, probe.nodeId);
          if (!applied) {
            failure = "invalid_node";
          }
        } else {
          const auto result = daw::connectPatcherNodes(
              scratch, probe.srcNodeId, probe.srcPortId, probe.dstNodeId,
              probe.dstPortId,
              static_cast<daw::PatcherPortKind>(probe.edgeKind));
          applied = result == daw::PatcherConnectResult::Ok;
          if (!applied) {
            failure = result == daw::PatcherConnectResult::InvalidNode
                          ? "invalid_node"
                          : (result == daw::PatcherConnectResult::InvalidPort
                                 ? "invalid_port"
                                 : (result == daw::PatcherConnectResult::Cycle
                                        ? "cycle"
                                        : "invalid_connection"));
          }
        }
        if (applied) {
          device->patcher = scratch.graph;
          runtime->trackSnapshot = buildTrackSnapshot(runtime->track);
        }
      }
    }
    if (!applied) {
      refuse(failure ? failure : "failed");
      return;
    }
    // The pool is DERIVED from the device graphs, so re-derive it — otherwise the edit is
    // saved and does nothing until the next load, which is its own kind of lie.
    const bool executing = reassemblePatcherFromDevices();
    DAW_EVENT("patcher_device_edit.applied")
        .field("track", probe.trackId)
        .field("device", deviceId)
        .field("op", daw::uiCommandTypeName(commandType))
        .field("node", newNodeId)
        .field("executing", executing);
    return;
  }
  daw::UiPatcherGraphCommandPayload graphPayload{};
  std::memcpy(&graphPayload, entry.payload, sizeof(graphPayload));
  constexpr uint16_t kGraphErrInvalidType = 1;
  constexpr uint16_t kGraphErrInvalidNode = 2;
  constexpr uint16_t kGraphErrCycle = 3;
  constexpr uint16_t kGraphErrAddFailed = 4;
  constexpr uint16_t kGraphErrInvalidConnection = 5;
  constexpr uint16_t kGraphErrInvalidPort = 6;
  if (commandType == daw::UiCommandType::AddPatcherNode) {
    if (graphPayload.nodeType >
        static_cast<uint32_t>(daw::kPatcherNodeTypeMax)) {
      emitPatcherGraphError(kGraphErrInvalidType,
                            graphPayload.trackId,
                            graphPayload.nodeId,
                            0,
                            0,
                            0,
                            0,
                            0);
      return;
    }
    const auto nodeId = addPatcherNode(
        patcherGraphState,
        static_cast<daw::PatcherNodeType>(graphPayload.nodeType));
    if (nodeId == std::numeric_limits<uint32_t>::max()) {
      emitPatcherGraphError(kGraphErrAddFailed,
                            graphPayload.trackId,
                            graphPayload.nodeId,
                            0,
                            0,
                            0,
                            0,
                            0);
      return;
    }
    patcherPoolEdited.store(true, std::memory_order_release);
    updatePatcherGraphSnapshot();
    emitPatcherGraphDelta(graphPayload.trackId,
                          0,
                          nodeId,
                          graphPayload.nodeType,
                          0,
                          0,
                          0,
                          0,
                          0);
    return;
  }
  if (commandType == daw::UiCommandType::RemovePatcherNode) {
    if (!removePatcherNode(patcherGraphState, graphPayload.nodeId)) {
      emitPatcherGraphError(kGraphErrInvalidNode,
                            graphPayload.trackId,
                            graphPayload.nodeId,
                            0,
                            0,
                            0,
                            0,
                            0);
      return;
    }
    patcherPoolEdited.store(true, std::memory_order_release);
    updatePatcherGraphSnapshot();
    emitPatcherGraphDelta(graphPayload.trackId,
                          1,
                          graphPayload.nodeId,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0);
    return;
  }
  if (commandType == daw::UiCommandType::ConnectPatcherNodes) {
    if (graphPayload.edgeKind >
        static_cast<uint32_t>(daw::PatcherPortKind::Control)) {
      emitPatcherGraphError(kGraphErrInvalidConnection,
                            graphPayload.trackId,
                            0,
                            graphPayload.srcNodeId,
                            graphPayload.dstNodeId,
                            graphPayload.srcPortId,
                            graphPayload.dstPortId,
                            graphPayload.edgeKind);
      return;
    }
    if (graphPayload.srcNodeId == graphPayload.dstNodeId) {
      emitPatcherGraphError(kGraphErrInvalidNode,
                            graphPayload.trackId,
                            0,
                            graphPayload.srcNodeId,
                            graphPayload.dstNodeId,
                            graphPayload.srcPortId,
                            graphPayload.dstPortId,
                            graphPayload.edgeKind);
      return;
    }
    const auto result = connectPatcherNodes(patcherGraphState,
                                            graphPayload.srcNodeId,
                                            graphPayload.srcPortId,
                                            graphPayload.dstNodeId,
                                            graphPayload.dstPortId,
                                            static_cast<daw::PatcherPortKind>(
                                                graphPayload.edgeKind));
    if (result != daw::PatcherConnectResult::Ok) {
      const uint16_t errorCode =
          result == daw::PatcherConnectResult::InvalidNode
              ? kGraphErrInvalidNode
              : (result == daw::PatcherConnectResult::InvalidPort
                     ? kGraphErrInvalidPort
                     : (result == daw::PatcherConnectResult::InvalidConnection
                            ? kGraphErrInvalidConnection
                            : kGraphErrCycle));
      emitPatcherGraphError(errorCode,
                            graphPayload.trackId,
                            0,
                            graphPayload.srcNodeId,
                            graphPayload.dstNodeId,
                            graphPayload.srcPortId,
                            graphPayload.dstPortId,
                            graphPayload.edgeKind);
      return;
    }
    patcherPoolEdited.store(true, std::memory_order_release);
    updatePatcherGraphSnapshot();
    emitPatcherGraphDelta(graphPayload.trackId,
                          2,
                          0,
                          0,
                          graphPayload.srcNodeId,
                          graphPayload.dstNodeId,
                          graphPayload.srcPortId,
                          graphPayload.dstPortId,
                          graphPayload.edgeKind);
    return;
  }
  }
}

void handleSetPatcherNodeConfig(PatcherCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& patcherGraphState = deps.patcherGraph.patcherGraphState;
  auto& patcherPoolEdited = deps.patcherGraph.patcherPoolEdited;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitPatcherGraphDelta = deps.emitPatcherGraphDelta;
  const auto& emitPatcherGraphError = deps.emitPatcherGraphError;
  const auto& reassemblePatcherFromDevices = deps.reassemblePatcherFromDevices;
  const auto& updatePatcherGraphSnapshot = deps.updatePatcherGraphSnapshot;
  {
  daw::UiPatcherNodeConfigPayload configPayload{};
  std::memcpy(&configPayload, entry.payload, sizeof(configPayload));
  // A DEVICE'S OWN GRAPH, if the caller named one — same flag encoding the graph commands
  // already use (bit 15 set, deviceId in bits 0-14).
  //
  // Without this the handler below edits `patcherGraphState`, the SHARED POOL, and since
  // patcher-is-a-device the pool is not what a project renders. So NO node's config was
  // editable by command on a per-device graph: not the new SliceSelect, and not euclidean's
  // hits or the LFO's rate either — verified with a pre-existing node type before assuming
  // it was the new one's wiring. Task #73 fixed exactly this for AddPatcherNode /
  // RemovePatcherNode / ConnectPatcherNodes, which share a different payload; this command
  // was never brought along.
  //
  // It reported SUCCESS the whole time: the pool has no node with that id, `updated` stays
  // false, and the refusal goes into an SHM diff no CLI surfaces.
  if ((configPayload.flags & daw::kUiPatcherFlagHasDeviceId) != 0) {
    const uint32_t deviceId =
        static_cast<uint32_t>(configPayload.flags & daw::kUiPatcherDeviceIdMask);
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, configPayload.trackId);
    auto refuseCfg = [&](const char* why) {
      DAW_EVENT("patcher_device_edit.rejected")
          .field("track", configPayload.trackId)
          .field("device", deviceId)
          .field("op", daw::uiCommandTypeName(commandType))
          .field("reason", why);
    };
    if (!runtime) {
      refuseCfg("no_such_track");
      return;
    }
    bool applied = false;
    const char* failure = nullptr;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      daw::Device* device = nullptr;
      for (auto& d : runtime->track.chain.devices) {
        if (d.id == deviceId) {
          device = &d;
          break;
        }
      }
      if (!device) {
        failure = "no_such_device";
      } else {
        daw::PatcherGraphState scratch;
        scratch.graph = device->patcher;
        applied = applyNodeConfigTo(scratch, configPayload, &failure);
        if (applied) {
          device->patcher = scratch.graph;
          runtime->trackSnapshot = buildTrackSnapshot(runtime->track);
        }
      }
    }
    if (!applied) {
      refuseCfg(failure ? failure : "failed");
      return;
    }
    // The pool is DERIVED from the device graphs, so re-derive it — otherwise the edit is
    // saved and does nothing until the next load.
    const bool executing = reassemblePatcherFromDevices();
    DAW_EVENT("patcher_device_edit.applied")
        .field("track", configPayload.trackId)
        .field("device", deviceId)
        .field("op", daw::uiCommandTypeName(commandType))
        .field("node", configPayload.nodeId)
        .field("executing", executing);
    return;
  }
  constexpr uint16_t kGraphErrInvalidType = 1;
  constexpr uint16_t kGraphErrInvalidNode = 2;
  // THE SHARED POOL. Reached only when the caller did NOT name a device; the device branch
  // above returns. Same decoder either way — see applyNodeConfigTo.
  const char* cfgFailure = nullptr;
  const bool updated =
      applyNodeConfigTo(patcherGraphState, configPayload, &cfgFailure);
  if (!updated) {
    const bool badType =
        cfgFailure != nullptr && std::strcmp(cfgFailure, "invalid_type") == 0;
    emitPatcherGraphError(badType ? kGraphErrInvalidType : kGraphErrInvalidNode,
                          configPayload.trackId,
                          configPayload.nodeId,
                          0,
                          0,
                          0,
                          0,
                          0);
    return;
  }
  patcherPoolEdited.store(true, std::memory_order_release);
  updatePatcherGraphSnapshot();
  emitPatcherGraphDelta(configPayload.trackId,
                        3,
                        configPayload.nodeId,
                        configPayload.configType,
                        0,
                        0,
                        0,
                        0,
                        0);
  return;
  }
}

void handleSavePatcherPreset(PatcherCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& patcherGraphState = deps.patcherGraph.patcherGraphState;
  const auto& emitUiDiff = deps.emitUiDiff;
  {
  daw::UiPatcherPresetCommandPayload presetPayload{};
  std::memcpy(&presetPayload, entry.payload, sizeof(presetPayload));
  std::string name(presetPayload.name,
                   strnlen(presetPayload.name, sizeof(presetPayload.name)));
  // Every exit from here reports the OUTCOME, including the early refusals. A caller that
  // gets nothing back cannot tell "refused" from "still working" from "written", and the
  // one thing it must not do is tell the user it saved.
  auto reportPreset = [&](bool ok, const std::string& why) {
    daw::UiPresetSavedPayload result{};
    result.diffType = static_cast<uint16_t>(daw::UiDiffType::PresetSaved);
    result.ok = ok ? 1u : 0u;
    const size_t n = std::min(name.size(), sizeof(result.name) - 1);
    std::memcpy(result.name, name.data(), n);
    daw::UiDiffPayload asDiff{};
    static_assert(sizeof(result) <= sizeof(asDiff),
                  "the preset result must fit the diff slot it rides");
    std::memcpy(&asDiff, &result, sizeof(result));
    emitUiDiff(asDiff);
    DAW_EVENT("patcher_preset.saved")
        .field("name", name)
        .field("ok", ok)
        .field("error", why);
  };
  if (name.empty()) {
    daw::LogLine() << "UI: SavePatcherPreset failed - empty name" << std::endl;
    reportPreset(false, "empty_name");
    return;
  }
  const std::string dir = daw::defaultPatcherPresetDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    daw::LogLine() << "UI: SavePatcherPreset failed - cannot create dir "
              << dir << std::endl;
    reportPreset(false, "cannot_create_dir");
    return;
  }
  const std::filesystem::path path =
      std::filesystem::path(dir) / (name + ".json");
  std::string error;
  if (!daw::savePatcherPreset(patcherGraphState,
                              path.string(),
                              &error)) {
    daw::LogLine() << "UI: SavePatcherPreset failed - " << error << std::endl;
    reportPreset(false, error);
  } else {
    daw::LogLine() << "UI: Saved patcher preset " << path.string() << std::endl;
    reportPreset(true, std::string());
  }
  return;
  }
}

}  // namespace daw::engine
