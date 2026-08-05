#include "apps/engine_load_patcher_pool.h"

// THE BODY BELOW IS VERBATIM — loadProjectFromPath's whole `if (deviceGraphCount >= 1) ... else`
// block, unedited, so the move is provable by diffing this range against the parent commit. Both
// branches travel together because they are one decision: assemble the pool from every device's
// graph, or fall back to the first single authored graph found.
#include <mutex>
#include <vector>

#include "apps/event_log.h"
#include "apps/patcher_assemble.h"

namespace daw::engine {

void loadPatcherGraphsFromDocument(LoadProjectDeps& deps, daw::ProjectDocument& document,
                                   size_t deviceGraphCount) {
  auto& patcherAssembledFromDevices = deps.engineState.patcherGraph.patcherAssembledFromDevices;
  auto& patcherGraphState = deps.engineState.patcherGraph.patcherGraphState;
  auto& updatePatcherGraphSnapshot = deps.updatePatcherGraphSnapshot;
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;

    if (deviceGraphCount >= 1) {
      daw::PatcherGraph pool;
      std::vector<DevOut> outputs;
      uint32_t base = 0;
      for (const auto& source : document.tracks) {
        daw::AssembledPatcher sub = daw::assemblePatcherPool(source.chain.devices);
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
          outputs.push_back({source.trackId, out.first, out.second + base});
        }
        base += static_cast<uint32_t>(sub.pool.nodes.size());
      }
      // A pool that will not build is a REPORTED failure, not a silent fallback. One
      // device with an invalid graph — an LFO wired to an event input, say — used to
      // fail the whole TRACK's assembly with nothing said, leave
      // patcherAssembledFromDevices false, and then the save below would park the pool
      // on the first device, overwriting its real graph and losing every other device's.
      // A bad edge in one device silently rewrote the user's project.
      const bool poolBuilt = !pool.nodes.empty() && daw::buildPatcherGraph(pool);
      if (!pool.nodes.empty() && !poolBuilt) {
        DAW_EVENT("project.patcher_assembly_failed")
            .field("nodes", static_cast<uint64_t>(pool.nodes.size()))
            .field("edges", static_cast<uint64_t>(pool.edges.size()))
            .field("action", "per_device_graphs_preserved_but_not_executing");
        daw::LogLine() << "Engine: patcher assembly FAILED (" << pool.nodes.size()
                  << " nodes, " << pool.edges.size()
                  << " edges) — one device's graph is invalid. The graphs are left "
                     "exactly as loaded and are NOT executing; run tools/daw_lint on "
                     "the project to find the bad edge." << std::endl;
      }
      if (poolBuilt) {
        {
          std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
          patcherGraphState.graph = std::move(pool);
          patcherGraphState.nextNodeId = base;
        }
        patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
        updatePatcherGraphSnapshot();
        // Repoint each device at its output node in the assembled pool, so the RT DFS
        // seeds from the right node AND the published patcherNodeId is a real pool node.
        //
        // This MUST write the DOCUMENT as well as the runtime: the per-track load below
        // rebuilds each chain from `source.chain` and installs it (runtime->track.chain =
        // std::move(loadedChain)), which would otherwise overwrite this repoint with the
        // device-local AUTHORED id. That is invisible for the first contributing device —
        // its pool block starts at offset 0, so authored == pooled — and wrong for every
        // device after it, which published an id belonging to ANOTHER device's subgraph.
        // Walking resolvedInputs back from it then recovered a neighbour's generator, so
        // per-device patcher scoping in the UI showed foreign nodes as unowned orphans.
        for (const auto& out : outputs) {
          for (auto& track : document.tracks) {
            if (track.trackId != out.trackId) {
              continue;
            }
            for (auto& d : track.chain.devices) {
              if (d.id == out.deviceId) {
                d.patcherNodeId = out.node;
                break;
              }
            }
            break;
          }
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
        patcherAssembledFromDevices.store(true, std::memory_order_release);
        DAW_EVENT("project.patcher_assembled")
            .field("devices", static_cast<uint64_t>(outputs.size()))
            .field("nodes", static_cast<uint64_t>(base));
      }
    } else {
      bool patcherLoaded = false;
      for (const auto& source : document.tracks) {
        if (patcherLoaded) {
          break;
        }
        for (const auto& device : source.chain.devices) {
          if (device.patcher.nodes.empty()) {
            continue;
          }
          daw::PatcherGraph loadedGraph = device.patcher;
          if (daw::buildPatcherGraph(loadedGraph)) {
            {
              std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
              patcherGraphState.graph = std::move(loadedGraph);
              uint32_t nextId = 0;
              for (const auto& node : patcherGraphState.graph.nodes) {
                nextId = std::max(nextId, node.id + 1);
              }
              patcherGraphState.nextNodeId = nextId;
            }
            patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
            updatePatcherGraphSnapshot();
            DAW_EVENT("project.patcher_loaded")
                .field("track", source.trackId)
                .field("device", device.id)
                .field("nodes", static_cast<uint64_t>(device.patcher.nodes.size()))
                .field("edges", static_cast<uint64_t>(device.patcher.edges.size()));
          } else {
            DAW_EVENT("project.patcher_invalid")
                .field("track", source.trackId)
                .field("device", device.id);
          }
          patcherLoaded = true;
          break;
        }
      }
    }
}

}  // namespace daw::engine
