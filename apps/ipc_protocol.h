#pragma once

#include <cstdint>

namespace daw {

constexpr uint32_t kControlMagic = 0x30485744;  // 'DWH0'
constexpr uint16_t kControlVersion = 1;

enum class ControlMessageType : uint16_t {
  Hello = 1,
  ProcessBlock = 2,
  Shutdown = 3,
  OpenEditor = 4,
  SetBypass = 5,
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
  uint32_t reserved = 0;
};

struct OpenEditorRequest {
  uint32_t pluginIndex = 0;
  uint32_t reserved = 0;
};

struct SetBypassRequest {
  uint32_t pluginIndex = 0;
  uint32_t bypass = 0;
};

}  // namespace daw
