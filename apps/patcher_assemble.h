#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "apps/device_chain.h"
#include "apps/patcher_graph.h"

namespace daw {

// The result of assembling a track's per-device patcher graphs into one pool.
struct AssembledPatcher {
  PatcherGraph pool;  // union of every contributing device's graph, re-id'd
  // (deviceId -> the device's output node id in `pool`), one per contributing
  // device, in chain order. The RT engine seeds its per-device DFS from these.
  std::vector<std::pair<uint32_t, uint32_t>> deviceOutputs;
  bool anyPerDevice = false;  // at least one device contributed a graph
};

// Picks the output node of a device's own patcher graph — the node whose events
// drive the device: its EventOut node if it has one, else a sink (a node with no
// outgoing edge), else the last node. Returns false only for an empty graph. Pure.
inline bool patcherGraphOutputNode(const PatcherGraph& g, uint32_t& outId) {
  if (g.nodes.empty()) {
    return false;
  }
  for (const auto& n : g.nodes) {
    if (n.type == PatcherNodeType::EventOut) {
      outId = n.id;
      return true;
    }
  }
  for (const auto& n : g.nodes) {
    bool hasOut = false;
    for (const auto& e : g.edges) {
      if (e.src.nodeId == n.id) {
        hasOut = true;
        break;
      }
    }
    if (!hasOut) {
      outId = n.id;
      return true;
    }
  }
  outId = g.nodes.back().id;
  return true;
}

// Merges each patcher-device's own `patcher` graph into one shared pool, giving
// every node a globally unique id (edges remapped to match) and reporting each
// device's output node id within the pool. Non-patcher devices, bypassed devices,
// and devices with an empty graph are skipped.
//
// This is ALL that per-device patcher execution needs beyond the existing engine:
// the RT scheduler already runs each device's subgraph independently by a DFS
// seeded from that device's output node (its patcherNodeId) over resolvedInputs,
// so once every device's graph lives in the pool as a disjoint node-set with the
// device pointing at its own output, the devices execute independently. Pure and
// deterministic (unit-tested off the audio thread); the caller runs
// buildPatcherGraph on the pool once after installing it.
inline AssembledPatcher assemblePatcherPool(const std::vector<Device>& devices) {
  AssembledPatcher out;
  uint32_t nextId = 0;
  for (const auto& device : devices) {
    if (device.kind != DeviceKind::PatcherEvent &&
        device.kind != DeviceKind::PatcherInstrument &&
        device.kind != DeviceKind::PatcherAudio) {
      continue;
    }
    if (device.bypass || device.patcher.nodes.empty()) {
      continue;
    }
    uint32_t deviceOut = 0;
    if (!patcherGraphOutputNode(device.patcher, deviceOut)) {
      continue;
    }
    // Remap this device's node ids to a fresh block above every prior device's,
    // so subgraphs never collide in the pool.
    std::vector<std::pair<uint32_t, uint32_t>> remap;  // oldId -> newId
    auto mapId = [&](uint32_t oldId) -> uint32_t {
      for (const auto& p : remap) {
        if (p.first == oldId) {
          return p.second;
        }
      }
      const uint32_t nid = nextId++;
      remap.push_back({oldId, nid});
      return nid;
    };
    for (const auto& n : device.patcher.nodes) {
      PatcherNode copy = n;
      copy.id = mapId(n.id);
      out.pool.nodes.push_back(copy);
    }
    for (const auto& e : device.patcher.edges) {
      PatcherEdge copy = e;
      copy.src.nodeId = mapId(e.src.nodeId);
      copy.dst.nodeId = mapId(e.dst.nodeId);
      out.pool.edges.push_back(copy);
    }
    out.deviceOutputs.push_back({device.id, mapId(deviceOut)});
    out.anyPerDevice = true;
  }
  return out;
}

}  // namespace daw
