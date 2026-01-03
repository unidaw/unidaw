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

enum class PatcherPortKind : uint8_t {
  Event = 0,
  Audio = 1,
  Control = 2,
};

enum class PatcherPortDirection : uint8_t {
  Input = 0,
  Output = 1,
};

enum class PatcherControlRate : uint8_t {
  Block = 0,
  Sample = 1,
};

constexpr uint32_t kPatcherEventInputPort = 0;
constexpr uint32_t kPatcherEventOutputPort = 1;
constexpr uint32_t kPatcherControlInputPort = 2;
constexpr uint32_t kPatcherControlOutputPort = 3;
constexpr uint32_t kPatcherAudioInputPort = 4;
constexpr uint32_t kPatcherAudioOutputPort = 5;

struct PatcherPortRef {
  uint32_t nodeId = 0;
  uint32_t portId = 0;
};

struct PatcherEdge {
  PatcherPortRef src;
  PatcherPortRef dst;
  PatcherPortKind kind = PatcherPortKind::Event;
};

struct PatcherNode {
  uint32_t id = 0;
  PatcherNodeType type = PatcherNodeType::RustKernel;
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
  std::vector<PatcherEdge> edges;
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

enum class PatcherConnectResult : uint8_t {
  Ok = 0,
  InvalidNode = 1,
  InvalidConnection = 2,
  InvalidPort = 3,
  Cycle = 4,
};

uint32_t addPatcherNode(PatcherGraphState& state, PatcherNodeType type);
PatcherConnectResult connectPatcherNodes(PatcherGraphState& state,
                                         uint32_t srcNodeId,
                                         uint32_t srcPortId,
                                         uint32_t dstNodeId,
                                         uint32_t dstPortId,
                                         PatcherPortKind kind);
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
