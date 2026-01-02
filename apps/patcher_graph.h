#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include "apps/patcher_abi.h"

namespace daw {

constexpr uint32_t kPatcherMaxNodes = 1024;

enum class PatcherNodeType : uint8_t {
  RustKernel = 0,
  Euclidean = 1,
  Passthrough = 2,
  AudioPassthrough = 3,
  Lfo = 4,
  RandomDegree = 5,
  EventOut = 6,
};

struct PatcherNode {
  uint32_t id = 0;
  PatcherNodeType type = PatcherNodeType::RustKernel;
  std::vector<uint32_t> inputs;  // Node ids.
  uint16_t depth = 0;
  bool hasEuclideanConfig = false;
  PatcherEuclideanConfig euclideanConfig{};
  bool hasLfoConfig = false;
  PatcherLfoConfig lfoConfig{};
  bool hasRandomDegreeConfig = false;
  PatcherRandomDegreeConfig randomDegreeConfig{};
};

struct PatcherGraph {
  std::vector<PatcherNode> nodes;
  std::vector<uint32_t> topoOrder;
  std::vector<uint16_t> depths;
  std::vector<std::vector<uint32_t>> resolvedInputs;
  std::vector<uint32_t> idToIndex;
  uint16_t maxDepth = 0;
};

bool buildPatcherGraph(PatcherGraph& graph);

constexpr uint32_t kPatcherInvalidNodeIndex =
    std::numeric_limits<uint32_t>::max();

struct PatcherGraphState {
  std::mutex mutex;
  PatcherGraph graph;
  std::atomic<uint32_t> version{0};
  uint32_t nextNodeId = 0;
};

uint32_t addPatcherNode(PatcherGraphState& state, PatcherNodeType type);
bool connectPatcherNodes(PatcherGraphState& state, uint32_t src, uint32_t dst);
bool removePatcherNode(PatcherGraphState& state, uint32_t nodeId);
bool setEuclideanConfig(PatcherGraphState& state,
                        uint32_t nodeId,
                        const PatcherEuclideanConfig& config);
bool setLfoConfig(PatcherGraphState& state,
                  uint32_t nodeId,
                  const PatcherLfoConfig& config);
bool setRandomDegreeConfig(PatcherGraphState& state,
                           uint32_t nodeId,
                           const PatcherRandomDegreeConfig& config);

}  // namespace daw
