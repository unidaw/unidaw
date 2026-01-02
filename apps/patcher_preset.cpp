#include "apps/patcher_preset.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_set>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace daw {
namespace {

constexpr uint32_t kPatcherPresetSchemaVersion = 1;

void setError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

const char* nodeTypeToString(PatcherNodeType type) {
  switch (type) {
    case PatcherNodeType::RustKernel:
      return "rust";
    case PatcherNodeType::Euclidean:
      return "euclidean";
    case PatcherNodeType::Passthrough:
      return "passthrough";
    case PatcherNodeType::AudioPassthrough:
      return "audio_passthrough";
    case PatcherNodeType::Lfo:
      return "lfo";
    case PatcherNodeType::RandomDegree:
      return "random_degree";
    case PatcherNodeType::EventOut:
      return "event_out";
  }
  return "unknown";
}

bool stringToNodeType(const std::string& value, PatcherNodeType& out) {
  if (value == "rust") {
    out = PatcherNodeType::RustKernel;
    return true;
  }
  if (value == "euclidean") {
    out = PatcherNodeType::Euclidean;
    return true;
  }
  if (value == "passthrough") {
    out = PatcherNodeType::Passthrough;
    return true;
  }
  if (value == "audio_passthrough") {
    out = PatcherNodeType::AudioPassthrough;
    return true;
  }
  if (value == "lfo") {
    out = PatcherNodeType::Lfo;
    return true;
  }
  if (value == "random_degree") {
    out = PatcherNodeType::RandomDegree;
    return true;
  }
  if (value == "event_out") {
    out = PatcherNodeType::EventOut;
    return true;
  }
  return false;
}

boost::property_tree::ptree serializeEuclidean(const PatcherEuclideanConfig& config) {
  boost::property_tree::ptree node;
  node.put("steps", config.steps);
  node.put("hits", config.hits);
  node.put("offset", config.offset);
  node.put("duration_ticks", config.duration_ticks);
  node.put("degree", config.degree);
  node.put("octave_offset", config.octave_offset);
  node.put("velocity", config.velocity);
  node.put("base_octave", config.base_octave);
  return node;
}

boost::property_tree::ptree serializeLfo(const PatcherLfoConfig& config) {
  boost::property_tree::ptree node;
  node.put("frequency_hz", config.frequency_hz);
  node.put("depth", config.depth);
  node.put("bias", config.bias);
  node.put("phase_offset", config.phase_offset);
  return node;
}

boost::property_tree::ptree serializeRandomDegree(const PatcherRandomDegreeConfig& config) {
  boost::property_tree::ptree node;
  node.put("degree", config.degree);
  node.put("velocity", config.velocity);
  node.put("duration_ticks", config.duration_ticks);
  return node;
}

void deserializeEuclidean(const boost::property_tree::ptree& node,
                          PatcherEuclideanConfig& config) {
  config.steps = node.get<uint32_t>("steps", config.steps);
  config.hits = node.get<uint32_t>("hits", config.hits);
  config.offset = node.get<uint32_t>("offset", config.offset);
  config.duration_ticks = node.get<uint64_t>("duration_ticks", config.duration_ticks);
  config.degree = node.get<uint8_t>("degree", config.degree);
  config.octave_offset = node.get<int8_t>("octave_offset", config.octave_offset);
  config.velocity = node.get<uint8_t>("velocity", config.velocity);
  config.base_octave = node.get<uint8_t>("base_octave", config.base_octave);
}

void deserializeLfo(const boost::property_tree::ptree& node,
                    PatcherLfoConfig& config) {
  config.frequency_hz = node.get<float>("frequency_hz", config.frequency_hz);
  config.depth = node.get<float>("depth", config.depth);
  config.bias = node.get<float>("bias", config.bias);
  config.phase_offset = node.get<float>("phase_offset", config.phase_offset);
}

void deserializeRandomDegree(const boost::property_tree::ptree& node,
                             PatcherRandomDegreeConfig& config) {
  config.degree = node.get<uint8_t>("degree", config.degree);
  config.velocity = node.get<uint8_t>("velocity", config.velocity);
  config.duration_ticks = node.get<uint64_t>("duration_ticks", config.duration_ticks);
}

}  // namespace

bool savePatcherPreset(const PatcherGraph& graph,
                       const std::string& path,
                       std::string* error) {
  boost::property_tree::ptree root;
  root.put("schema_version", kPatcherPresetSchemaVersion);

  boost::property_tree::ptree nodesTree;
  for (const auto& node : graph.nodes) {
    boost::property_tree::ptree nodeTree;
    nodeTree.put("id", node.id);
    nodeTree.put("type", nodeTypeToString(node.type));

    boost::property_tree::ptree inputsTree;
    for (uint32_t input : node.inputs) {
      boost::property_tree::ptree inputTree;
      inputTree.put("", input);
      inputsTree.push_back(std::make_pair("", inputTree));
    }
    nodeTree.add_child("inputs", inputsTree);

    if (node.hasEuclideanConfig) {
      nodeTree.add_child("euclidean", serializeEuclidean(node.euclideanConfig));
    }
    if (node.hasLfoConfig) {
      nodeTree.add_child("lfo", serializeLfo(node.lfoConfig));
    }
    if (node.hasRandomDegreeConfig) {
      nodeTree.add_child("random_degree", serializeRandomDegree(node.randomDegreeConfig));
    }

    nodesTree.push_back(std::make_pair("", nodeTree));
  }
  root.add_child("nodes", nodesTree);

  try {
    boost::property_tree::write_json(path, root);
  } catch (const std::exception& ex) {
    setError(error, ex.what());
    return false;
  }

  return true;
}

bool savePatcherPreset(PatcherGraphState& state,
                       const std::string& path,
                       std::string* error) {
  std::lock_guard<std::mutex> lock(state.mutex);
  return savePatcherPreset(state.graph, path, error);
}

bool loadPatcherPreset(PatcherGraph& graph,
                       const std::string& path,
                       std::string* error) {
  boost::property_tree::ptree root;
  try {
    boost::property_tree::read_json(path, root);
  } catch (const std::exception& ex) {
    setError(error, ex.what());
    return false;
  }

  const uint32_t schemaVersion = root.get<uint32_t>("schema_version", 0);
  if (schemaVersion != kPatcherPresetSchemaVersion) {
    setError(error, "unsupported patcher preset schema version");
    return false;
  }

  auto nodesOpt = root.get_child_optional("nodes");
  if (!nodesOpt) {
    setError(error, "patcher preset missing nodes");
    return false;
  }

  std::vector<PatcherNode> nodes;
  std::unordered_set<uint32_t> seenIds;
  for (const auto& entry : *nodesOpt) {
    const auto& nodeTree = entry.second;
    const auto nodeIdOpt = nodeTree.get_optional<uint32_t>("id");
    if (!nodeIdOpt) {
      setError(error, "patcher preset node missing id");
      return false;
    }
    const uint32_t nodeId = *nodeIdOpt;
    if (!seenIds.insert(nodeId).second) {
      setError(error, "patcher preset contains duplicate node id");
      return false;
    }

    const std::string typeStr = nodeTree.get<std::string>("type", "");
    PatcherNodeType type = PatcherNodeType::RustKernel;
    if (!stringToNodeType(typeStr, type)) {
      setError(error, "patcher preset contains unknown node type");
      return false;
    }

    PatcherNode node;
    node.id = nodeId;
    node.type = type;

    if (auto inputsTree = nodeTree.get_child_optional("inputs")) {
      for (const auto& inputEntry : *inputsTree) {
        node.inputs.push_back(inputEntry.second.get_value<uint32_t>());
      }
    }

    if (auto euclidTree = nodeTree.get_child_optional("euclidean")) {
      node.hasEuclideanConfig = true;
      deserializeEuclidean(*euclidTree, node.euclideanConfig);
    }
    if (auto lfoTree = nodeTree.get_child_optional("lfo")) {
      node.hasLfoConfig = true;
      deserializeLfo(*lfoTree, node.lfoConfig);
    }
    if (auto randomTree = nodeTree.get_child_optional("random_degree")) {
      node.hasRandomDegreeConfig = true;
      deserializeRandomDegree(*randomTree, node.randomDegreeConfig);
    }

    nodes.push_back(node);
  }

  graph.nodes = std::move(nodes);
  if (!buildPatcherGraph(graph)) {
    setError(error, "patcher preset graph is invalid");
    return false;
  }

  return true;
}

bool loadPatcherPreset(PatcherGraphState& state,
                       const std::string& path,
                       std::string* error) {
  PatcherGraph loaded;
  if (!loadPatcherPreset(loaded, path, error)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.graph = std::move(loaded);
    uint32_t nextId = 0;
    for (const auto& node : state.graph.nodes) {
      nextId = std::max(nextId, node.id + 1);
    }
    state.nextNodeId = nextId;
  }
  state.version.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

}  // namespace daw
