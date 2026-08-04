#include "engine_patcher_assemble.h"

#include "event_log.h"
#include "patcher_assemble.h"

namespace daw::engine {

void updatePatcherGraphSnapshot(PatcherGraphOwner& patcherGraph) {

    auto snapshot = std::make_shared<daw::PatcherGraph>();
    {
      std::lock_guard<std::mutex> lock(patcherGraph.patcherGraphState.mutex);
      *snapshot = patcherGraph.patcherGraphState.graph;
    }
    std::atomic_store_explicit(&patcherGraph.patcherGraphSnapshot,
                               std::move(snapshot),
                               std::memory_order_release);
}

bool reassemblePatcherFromDevices(PatcherAssembleDeps& deps) {
  auto& patcherGraph = deps.patcherGraph;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& snapshotTracks = deps.snapshotTracks;

    daw::PatcherGraph pool;
    std::vector<DevOut> outputs;
    uint32_t base = 0;
    for (auto* rt : snapshotTracks()) {
      if (!rt || rt->removed.load(std::memory_order_acquire)) {
        continue;
      }
      std::vector<daw::Device> devices;
      {
        std::lock_guard<std::mutex> lock(rt->trackMutex);
        devices = rt->track.chain.devices;
      }
      daw::AssembledPatcher sub = daw::assemblePatcherPool(devices);
      if (!sub.anyPerDevice) {
        continue;
      }
      for (auto node : sub.pool.nodes) {
        node.id += base;
        pool.nodes.push_back(node);
      }
      for (auto edge : sub.pool.edges) {
        edge.src.nodeId += base;
        edge.dst.nodeId += base;
        pool.edges.push_back(edge);
      }
      for (const auto& out : sub.deviceOutputs) {
        outputs.push_back({rt->trackId, out.first, out.second + base});
      }
      base += static_cast<uint32_t>(sub.pool.nodes.size());
    }
    if (pool.nodes.empty()) {
      return false;
    }
    if (!daw::buildPatcherGraph(pool)) {
      DAW_EVENT("patcher.reassembly_failed")
          .field("nodes", static_cast<uint64_t>(pool.nodes.size()))
          .field("edges", static_cast<uint64_t>(pool.edges.size()))
          .field("action", "previous_pool_left_running");
      daw::LogLine() << "Engine: patcher re-assembly FAILED (" << pool.nodes.size()
                << " nodes) — one device's graph is invalid. The edit is kept, the PREVIOUS "
                   "pool is still executing; run tools/daw_lint to find the bad edge."
                << std::endl;
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(patcherGraph.patcherGraphState.mutex);
      patcherGraph.patcherGraphState.graph = std::move(pool);
      patcherGraph.patcherGraphState.nextNodeId = base;
    }
    patcherGraph.patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
    updatePatcherGraphSnapshot(deps.patcherGraph);
    // Repoint each device at its output node in the new pool, so the RT DFS seeds from the right
    // node and the published patcherNodeId names a real pool node. Skipping this is invisible for
    // the FIRST contributing device (its block starts at offset 0, so authored == pooled) and
    // wrong for every device after it — which is exactly the bug that made per-device scoping in
    // the UI show foreign nodes as unowned orphans.
    for (const auto& out : outputs) {
      TrackRuntime* rt = daw::engine::trackAt(tracks, tracksMutex, out.trackId);
      if (!rt) {
        continue;
      }
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      for (auto& d : rt->track.chain.devices) {
        if (d.id == out.deviceId) {
          d.patcherNodeId = out.node;
          break;
        }
      }
    }
    patcherGraph.patcherAssembledFromDevices.store(true, std::memory_order_release);
    DAW_EVENT("patcher.reassembled")
        .field("devices", static_cast<uint64_t>(outputs.size()))
        .field("nodes", static_cast<uint64_t>(base));
    return true;
}

}  // namespace daw::engine
