#include "apps/patcher_graph.h"

#include <algorithm>
#include <queue>

namespace daw {

bool buildPatcherGraph(PatcherGraph& graph) {
  const size_t nodeCount = graph.nodes.size();
  graph.topoOrder.clear();
  graph.depths.assign(nodeCount, 0);
  graph.maxDepth = 0;
  if (nodeCount > kPatcherMaxNodes) {
    return false;
  }
  if (nodeCount == 0) {
    return true;
  }
  std::vector<uint32_t> indegree(nodeCount, 0);
  std::vector<std::vector<uint32_t>> outputs(nodeCount);
  for (size_t i = 0; i < nodeCount; ++i) {
    const auto& node = graph.nodes[i];
    for (uint32_t input : node.inputs) {
      if (input >= nodeCount) {
        return false;
      }
      indegree[i]++;
      outputs[input].push_back(static_cast<uint32_t>(i));
    }
  }
  std::queue<uint32_t> ready;
  for (uint32_t i = 0; i < nodeCount; ++i) {
    if (indegree[i] == 0) {
      ready.push(i);
    }
  }
  while (!ready.empty()) {
    const uint32_t current = ready.front();
    ready.pop();
    graph.topoOrder.push_back(current);
    const uint8_t baseDepth = graph.depths[current];
    graph.maxDepth = std::max(graph.maxDepth, baseDepth);
    for (uint32_t next : outputs[current]) {
      if (graph.depths[next] < baseDepth + 1) {
        graph.depths[next] = static_cast<uint8_t>(baseDepth + 1);
        graph.maxDepth = std::max(graph.maxDepth, graph.depths[next]);
      }
      if (indegree[next] == 0) {
        continue;
      }
      indegree[next]--;
      if (indegree[next] == 0) {
        ready.push(next);
      }
    }
  }
  return graph.topoOrder.size() == nodeCount;
}

uint32_t addPatcherNode(PatcherGraphState& state, PatcherNodeType type) {
  std::lock_guard<std::mutex> lock(state.mutex);
  PatcherNode node;
  node.id = static_cast<uint32_t>(state.graph.nodes.size());
  node.type = type;
  state.graph.nodes.push_back(node);
  if (!buildPatcherGraph(state.graph)) {
    state.graph.nodes.pop_back();
    return std::numeric_limits<uint32_t>::max();
  }
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return node.id;
}

bool connectPatcherNodes(PatcherGraphState& state, uint32_t src, uint32_t dst) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (src >= state.graph.nodes.size() || dst >= state.graph.nodes.size()) {
    return false;
  }
  auto& node = state.graph.nodes[dst];
  if (std::find(node.inputs.begin(), node.inputs.end(), src) != node.inputs.end()) {
    return true;
  }
  node.inputs.push_back(src);
  if (!buildPatcherGraph(state.graph)) {
    node.inputs.pop_back();
    return false;
  }
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool removePatcherNode(PatcherGraphState& state, uint32_t nodeId) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (nodeId >= state.graph.nodes.size()) {
    return false;
  }
  state.graph.nodes.erase(state.graph.nodes.begin() + static_cast<long>(nodeId));
  for (uint32_t i = 0; i < state.graph.nodes.size(); ++i) {
    state.graph.nodes[i].id = i;
    auto& inputs = state.graph.nodes[i].inputs;
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [&](uint32_t input) { return input == nodeId; }),
                 inputs.end());
    for (auto& input : inputs) {
      if (input > nodeId) {
        input -= 1;
      }
    }
  }
  if (!buildPatcherGraph(state.graph)) {
    return false;
  }
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool setEuclideanConfig(PatcherGraphState& state,
                        uint32_t nodeId,
                        const PatcherEuclideanConfig& config) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (nodeId >= state.graph.nodes.size()) {
    return false;
  }
  auto& node = state.graph.nodes[nodeId];
  if (node.type != PatcherNodeType::Euclidean) {
    return false;
  }
  node.hasEuclideanConfig = true;
  node.euclideanConfig = config;
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

}  // namespace daw
