#include "apps/patcher_graph.h"

#include <algorithm>
#include <optional>
#include <queue>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/range/iterator_range.hpp>

namespace daw {

namespace {

uint32_t maxNodeId(const PatcherGraph& graph) {
  uint32_t maxId = 0;
  for (const auto& node : graph.nodes) {
    maxId = std::max(maxId, node.id);
  }
  return maxId;
}

std::optional<uint32_t> findNodeIndex(const std::vector<PatcherNode>& nodes,
                                      uint32_t nodeId) {
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].id == nodeId) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

bool buildPatcherGraph(PatcherGraph& graph) {
  const size_t nodeCount = graph.nodes.size();
  graph.topoOrder.clear();
  graph.depths.assign(nodeCount, 0);
  graph.resolvedInputs.assign(nodeCount, {});
  graph.idToIndex.clear();
  graph.maxDepth = 0;
  if (nodeCount > kPatcherMaxNodes) {
    return false;
  }
  if (nodeCount == 0) {
    return true;
  }
  const uint32_t maxId = maxNodeId(graph);
  graph.idToIndex.assign(static_cast<size_t>(maxId) + 1,
                         kPatcherInvalidNodeIndex);
  for (uint32_t i = 0; i < nodeCount; ++i) {
    const uint32_t nodeId = graph.nodes[i].id;
    if (nodeId >= graph.idToIndex.size()) {
      return false;
    }
    if (graph.idToIndex[nodeId] != kPatcherInvalidNodeIndex) {
      return false;
    }
    graph.idToIndex[nodeId] = i;
  }
  using Graph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS>;
  Graph dag(nodeCount);
  for (size_t i = 0; i < nodeCount; ++i) {
    const auto& node = graph.nodes[i];
    for (uint32_t input : node.inputs) {
      if (input >= graph.idToIndex.size()) {
        return false;
      }
      const uint32_t inputIndex = graph.idToIndex[input];
      if (inputIndex == kPatcherInvalidNodeIndex) {
        return false;
      }
      boost::add_edge(static_cast<size_t>(inputIndex), i, dag);
      graph.resolvedInputs[i].push_back(inputIndex);
    }
  }

  std::vector<Graph::vertex_descriptor> topo;
  try {
    boost::topological_sort(dag, std::back_inserter(topo));
  } catch (const boost::not_a_dag&) {
    return false;
  }

  graph.topoOrder.reserve(nodeCount);
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    graph.topoOrder.push_back(static_cast<uint32_t>(*it));
  }

  for (uint32_t nodeIndex : graph.topoOrder) {
    const uint16_t baseDepth = graph.depths[nodeIndex];
    graph.maxDepth = std::max(graph.maxDepth, baseDepth);
    for (const auto edge : boost::make_iterator_range(
             boost::out_edges(nodeIndex, dag))) {
      const auto target = boost::target(edge, dag);
      if (graph.depths[target] < baseDepth + 1) {
        graph.depths[target] = static_cast<uint16_t>(baseDepth + 1);
        graph.maxDepth = std::max(graph.maxDepth, graph.depths[target]);
      }
    }
  }
  for (uint32_t i = 0; i < nodeCount; ++i) {
    graph.nodes[i].depth = graph.depths[i];
  }
  return graph.topoOrder.size() == nodeCount;
}

uint32_t addPatcherNode(PatcherGraphState& state, PatcherNodeType type) {
  std::lock_guard<std::mutex> lock(state.mutex);
  const uint32_t maxId = maxNodeId(state.graph);
  if (state.nextNodeId <= maxId) {
    state.nextNodeId = maxId + 1;
  }
  PatcherNode node;
  node.id = state.nextNodeId++;
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
  const auto srcIndex = findNodeIndex(state.graph.nodes, src);
  const auto dstIndex = findNodeIndex(state.graph.nodes, dst);
  if (!srcIndex || !dstIndex) {
    return false;
  }
  auto& node = state.graph.nodes[*dstIndex];
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
  const auto nodeIndex = findNodeIndex(state.graph.nodes, nodeId);
  if (!nodeIndex) {
    return false;
  }
  state.graph.nodes.erase(state.graph.nodes.begin() + static_cast<long>(*nodeIndex));
  for (uint32_t i = 0; i < state.graph.nodes.size(); ++i) {
    auto& inputs = state.graph.nodes[i].inputs;
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [&](uint32_t input) { return input == nodeId; }),
                 inputs.end());
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

bool setLfoConfig(PatcherGraphState& state,
                  uint32_t nodeId,
                  const PatcherLfoConfig& config) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (nodeId >= state.graph.nodes.size()) {
    return false;
  }
  auto& node = state.graph.nodes[nodeId];
  if (node.type != PatcherNodeType::Lfo) {
    return false;
  }
  node.hasLfoConfig = true;
  node.lfoConfig = config;
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool setRandomDegreeConfig(PatcherGraphState& state,
                           uint32_t nodeId,
                           const PatcherRandomDegreeConfig& config) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (nodeId >= state.graph.nodes.size()) {
    return false;
  }
  auto& node = state.graph.nodes[nodeId];
  if (node.type != PatcherNodeType::RandomDegree) {
    return false;
  }
  node.hasRandomDegreeConfig = true;
  node.randomDegreeConfig = config;
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

}  // namespace daw
