#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "apps/patcher_abi.h"

namespace daw {

constexpr uint32_t kPatcherMaxNodes = 8;

enum class PatcherNodeType : uint8_t {
  RustKernel = 0,
  Euclidean = 1,
  Passthrough = 2,
  AudioPassthrough = 3,
};

struct PatcherNode {
  uint32_t id = 0;
  PatcherNodeType type = PatcherNodeType::RustKernel;
  std::vector<uint32_t> inputs;
  uint8_t depth = 0;
  bool hasEuclideanConfig = false;
  PatcherEuclideanConfig euclideanConfig{};
};

struct PatcherGraph {
  std::vector<PatcherNode> nodes;
  std::vector<uint32_t> topoOrder;
  std::vector<uint8_t> depths;
  uint8_t maxDepth = 0;
};

bool buildPatcherGraph(PatcherGraph& graph);

struct PatcherGraphState {
  std::mutex mutex;
  PatcherGraph graph;
  std::atomic<uint32_t> version{0};
};

uint32_t addPatcherNode(PatcherGraphState& state, PatcherNodeType type);
bool connectPatcherNodes(PatcherGraphState& state, uint32_t src, uint32_t dst);
bool removePatcherNode(PatcherGraphState& state, uint32_t nodeId);
bool setEuclideanConfig(PatcherGraphState& state,
                        uint32_t nodeId,
                        const PatcherEuclideanConfig& config);

}  // namespace daw
