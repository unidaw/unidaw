#include "apps/patcher_graph.h"

#include <algorithm>
#include <optional>
#include <queue>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/range/iterator_range.hpp>

namespace daw {

namespace {

struct PatcherPortDesc {
  uint32_t id = 0;
  PatcherPortKind kind = PatcherPortKind::Event;
  PatcherPortDirection direction = PatcherPortDirection::Input;
  PatcherControlRate controlRate = PatcherControlRate::Block;
  uint16_t channelCount = 0;
};

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

bool nodeAcceptsEventInput(PatcherNodeType type) {
  switch (type) {
    case PatcherNodeType::RustKernel:
    case PatcherNodeType::Passthrough:
    case PatcherNodeType::RandomDegree:
    case PatcherNodeType::EventOut:
      return true;
    case PatcherNodeType::Euclidean:
    case PatcherNodeType::AudioPassthrough:
    case PatcherNodeType::Lfo:
      return false;
  }
  return false;
}

bool nodeProvidesEventOutput(PatcherNodeType type) {
  switch (type) {
    case PatcherNodeType::RustKernel:
    case PatcherNodeType::Euclidean:
    case PatcherNodeType::Passthrough:
    case PatcherNodeType::RandomDegree:
      return true;
    case PatcherNodeType::AudioPassthrough:
    case PatcherNodeType::Lfo:
    case PatcherNodeType::EventOut:
      return false;
  }
  return false;
}

const std::vector<PatcherPortDesc>& portsForNode(PatcherNodeType type) {
  static const std::vector<PatcherPortDesc> kRustPorts = {
      {kPatcherEventInputPort, PatcherPortKind::Event, PatcherPortDirection::Input,
       PatcherControlRate::Block, 0},
      {kPatcherEventOutputPort, PatcherPortKind::Event, PatcherPortDirection::Output,
       PatcherControlRate::Block, 0},
      {kPatcherControlInputPort, PatcherPortKind::Control, PatcherPortDirection::Input,
       PatcherControlRate::Block, 0},
      {kPatcherControlOutputPort, PatcherPortKind::Control, PatcherPortDirection::Output,
       PatcherControlRate::Block, 0},
  };
  static const std::vector<PatcherPortDesc> kEuclideanPorts = {
      {kPatcherEventOutputPort, PatcherPortKind::Event, PatcherPortDirection::Output,
       PatcherControlRate::Block, 0},
  };
  static const std::vector<PatcherPortDesc> kPassthroughPorts = {
      {kPatcherEventInputPort, PatcherPortKind::Event, PatcherPortDirection::Input,
       PatcherControlRate::Block, 0},
      {kPatcherEventOutputPort, PatcherPortKind::Event, PatcherPortDirection::Output,
       PatcherControlRate::Block, 0},
  };
  static const std::vector<PatcherPortDesc> kAudioPassthroughPorts = {
      {kPatcherAudioInputPort, PatcherPortKind::Audio, PatcherPortDirection::Input,
       PatcherControlRate::Block, 2},
      {kPatcherAudioOutputPort, PatcherPortKind::Audio, PatcherPortDirection::Output,
       PatcherControlRate::Block, 2},
  };
  static const std::vector<PatcherPortDesc> kLfoPorts = {
      {kPatcherControlOutputPort, PatcherPortKind::Control, PatcherPortDirection::Output,
       PatcherControlRate::Sample, 0},
  };
  static const std::vector<PatcherPortDesc> kRandomDegreePorts = {
      {kPatcherEventInputPort, PatcherPortKind::Event, PatcherPortDirection::Input,
       PatcherControlRate::Block, 0},
      {kPatcherEventOutputPort, PatcherPortKind::Event, PatcherPortDirection::Output,
       PatcherControlRate::Block, 0},
  };
  static const std::vector<PatcherPortDesc> kEventOutPorts = {
      {kPatcherEventInputPort, PatcherPortKind::Event, PatcherPortDirection::Input,
       PatcherControlRate::Block, 0},
  };
  switch (type) {
    case PatcherNodeType::RustKernel:
      return kRustPorts;
    case PatcherNodeType::Euclidean:
      return kEuclideanPorts;
    case PatcherNodeType::Passthrough:
      return kPassthroughPorts;
    case PatcherNodeType::AudioPassthrough:
      return kAudioPassthroughPorts;
    case PatcherNodeType::Lfo:
      return kLfoPorts;
    case PatcherNodeType::RandomDegree:
      return kRandomDegreePorts;
    case PatcherNodeType::EventOut:
      return kEventOutPorts;
  }
  return kRustPorts;
}

const PatcherPortDesc* findPort(PatcherNodeType type,
                                uint32_t portId,
                                PatcherPortDirection direction) {
  const auto& ports = portsForNode(type);
  for (const auto& port : ports) {
    if (port.id == portId && port.direction == direction) {
      return &port;
    }
  }
  return nullptr;
}

bool isValidEdge(const PatcherNode& src,
                 const PatcherNode& dst,
                 const PatcherEdge& edge) {
  const auto* srcPort =
      findPort(src.type, edge.src.portId, PatcherPortDirection::Output);
  if (!srcPort) {
    return false;
  }
  const auto* dstPort =
      findPort(dst.type, edge.dst.portId, PatcherPortDirection::Input);
  if (!dstPort) {
    return false;
  }
  if (srcPort->kind != dstPort->kind) {
    return false;
  }
  if (srcPort->kind != edge.kind) {
    return false;
  }
  if (edge.kind == PatcherPortKind::Control &&
      srcPort->controlRate != dstPort->controlRate) {
    return false;
  }
  if (edge.kind == PatcherPortKind::Audio) {
    if (srcPort->channelCount == 0 || dstPort->channelCount == 0) {
      return true;
    }
    if (srcPort->channelCount != dstPort->channelCount) {
      return false;
    }
  }
  if (edge.kind == PatcherPortKind::Event &&
      (!nodeProvidesEventOutput(src.type) || !nodeAcceptsEventInput(dst.type))) {
    return false;
  }
  return true;
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
    return graph.edges.empty();
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
  for (const auto& edge : graph.edges) {
    if (edge.src.nodeId >= graph.idToIndex.size() ||
        edge.dst.nodeId >= graph.idToIndex.size()) {
      return false;
    }
    const uint32_t srcIndex = graph.idToIndex[edge.src.nodeId];
    const uint32_t dstIndex = graph.idToIndex[edge.dst.nodeId];
    if (srcIndex == kPatcherInvalidNodeIndex ||
        dstIndex == kPatcherInvalidNodeIndex) {
      return false;
    }
    if (!isValidEdge(graph.nodes[srcIndex], graph.nodes[dstIndex], edge)) {
      return false;
    }
    boost::add_edge(static_cast<size_t>(srcIndex), dstIndex, dag);
    if (edge.kind == PatcherPortKind::Event) {
      graph.resolvedInputs[dstIndex].push_back(srcIndex);
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

PatcherConnectResult connectPatcherNodes(PatcherGraphState& state,
                                         uint32_t srcNodeId,
                                         uint32_t srcPortId,
                                         uint32_t dstNodeId,
                                         uint32_t dstPortId,
                                         PatcherPortKind kind) {
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto srcIndex = findNodeIndex(state.graph.nodes, srcNodeId);
  const auto dstIndex = findNodeIndex(state.graph.nodes, dstNodeId);
  if (!srcIndex || !dstIndex) {
    return PatcherConnectResult::InvalidNode;
  }
  PatcherEdge edge;
  edge.src = {srcNodeId, srcPortId};
  edge.dst = {dstNodeId, dstPortId};
  edge.kind = kind;
  if (!findPort(state.graph.nodes[*srcIndex].type,
                srcPortId,
                PatcherPortDirection::Output) ||
      !findPort(state.graph.nodes[*dstIndex].type,
                dstPortId,
                PatcherPortDirection::Input)) {
    return PatcherConnectResult::InvalidPort;
  }
  if (!isValidEdge(state.graph.nodes[*srcIndex],
                   state.graph.nodes[*dstIndex],
                   edge)) {
    return PatcherConnectResult::InvalidConnection;
  }
  auto sameEdge = [&](const PatcherEdge& existing) {
    return existing.src.nodeId == edge.src.nodeId &&
        existing.src.portId == edge.src.portId &&
        existing.dst.nodeId == edge.dst.nodeId &&
        existing.dst.portId == edge.dst.portId &&
        existing.kind == edge.kind;
  };
  if (std::any_of(state.graph.edges.begin(),
                  state.graph.edges.end(),
                  sameEdge)) {
    return PatcherConnectResult::Ok;
  }
  state.graph.edges.push_back(edge);
  if (!buildPatcherGraph(state.graph)) {
    state.graph.edges.pop_back();
    return PatcherConnectResult::Cycle;
  }
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return PatcherConnectResult::Ok;
}

bool removePatcherNode(PatcherGraphState& state, uint32_t nodeId) {
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto nodeIndex = findNodeIndex(state.graph.nodes, nodeId);
  if (!nodeIndex) {
    return false;
  }
  state.graph.nodes.erase(state.graph.nodes.begin() + static_cast<long>(*nodeIndex));
  state.graph.edges.erase(
      std::remove_if(state.graph.edges.begin(),
                     state.graph.edges.end(),
                     [&](const PatcherEdge& edge) {
                       return edge.src.nodeId == nodeId ||
                           edge.dst.nodeId == nodeId;
                     }),
      state.graph.edges.end());
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
  const auto nodeIndex = findNodeIndex(state.graph.nodes, nodeId);
  if (!nodeIndex) {
    return false;
  }
  auto& node = state.graph.nodes[*nodeIndex];
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
  const auto nodeIndex = findNodeIndex(state.graph.nodes, nodeId);
  if (!nodeIndex) {
    return false;
  }
  auto& node = state.graph.nodes[*nodeIndex];
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
  const auto nodeIndex = findNodeIndex(state.graph.nodes, nodeId);
  if (!nodeIndex) {
    return false;
  }
  auto& node = state.graph.nodes[*nodeIndex];
  if (node.type != PatcherNodeType::RandomDegree) {
    return false;
  }
  node.hasRandomDegreeConfig = true;
  node.randomDegreeConfig = config;
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

}  // namespace daw
