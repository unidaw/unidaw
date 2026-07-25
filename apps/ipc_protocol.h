#pragma once

#include <cstdint>

namespace daw {

constexpr uint32_t kControlMagic = 0x30485744;  // 'DWH0'
// 2: ProcessBlockRequest carries transport position for the play head.
constexpr uint16_t kControlVersion = 2;

enum class ControlMessageType : uint16_t {
  Hello = 1,
  ProcessBlock = 2,
  Shutdown = 3,
  OpenEditor = 4,
  SetBypass = 5,
  // Opaque plugin state. Payload is StateHeader followed by the blob; the
  // reply to GetState reuses the same shape. Without these a chain edit
  // restarts the host and the sound is gone.
  GetState = 6,
  SetState = 7,
  // Reconcile the plugin chain to a new path list in place, reusing unchanged
  // instances, so a chain edit does not restart the whole host and drop audio.
  // Payload is ChainHeader followed by `count` null-terminated UTF-8 paths.
  SetChain = 8,
};

struct StateHeader {
  uint32_t pluginIndex = 0;
  uint32_t byteCount = 0;
};

struct ChainHeader {
  uint32_t count = 0;      // number of null-terminated paths that follow
  uint32_t byteCount = 0;  // total bytes of the path block
};

struct ControlHeader {
  uint32_t magic = kControlMagic;
  uint16_t version = kControlVersion;
  uint16_t type = 0;
  uint32_t size = 0;
  uint32_t reserved = 0;
};

struct HelloRequest {
  uint32_t blockSize = 0;
  uint32_t numChannelsIn = 0;
  uint32_t numChannelsOut = 0;
  uint32_t numBlocks = 0;
  uint32_t ringStdCapacity = 0;
  uint32_t ringCtrlCapacity = 0;
  uint32_t ringUiCapacity = 0;
  double sampleRate = 0.0;
};

struct HelloResponse {
  uint64_t shmSizeBytes = 0;
  char shmName[64]{};
};

struct ProcessBlockRequest {
  uint32_t blockId = 0;
  uint64_t engineSampleStart = 0;
  uint64_t pluginSampleStart = 0;
  uint16_t segmentStart = 0;
  uint16_t segmentLength = 0;
  uint32_t flags = 0;  // bit 0: transport is playing
  // Musical position for this block. A hosted plugin has no clock of its own,
  // so without these every tempo-synced effect free-runs.
  double bpm = 120.0;
  double ppqPosition = 0.0;
  double ppqPositionOfLastBarStart = 0.0;
  uint32_t timeSigNumerator = 4;
  uint32_t timeSigDenominator = 4;
};

constexpr uint32_t kProcessBlockFlagPlaying = 1u << 0;

struct OpenEditorRequest {
  uint32_t pluginIndex = 0;
  uint32_t reserved = 0;
};

struct SetBypassRequest {
  uint32_t pluginIndex = 0;
  uint32_t bypass = 0;
};

}  // namespace daw
