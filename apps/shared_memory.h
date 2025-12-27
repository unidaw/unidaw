#pragma once

#include <atomic>
#include <cstdint>

namespace daw {

constexpr uint32_t kShmMagic = 0x30415744;  // 'DAW0'
constexpr uint16_t kShmVersion = 2;

constexpr uint32_t kUiMaxTracks = 8;

struct alignas(64) ShmHeader {
  uint32_t magic = kShmMagic;
  uint16_t version = kShmVersion;
  uint16_t flags = 0;
  uint32_t blockSize = 0;
  double sampleRate = 0.0;
  uint32_t numChannelsIn = 0;
  uint32_t numChannelsOut = 0;
  uint32_t numBlocks = 0;
  uint32_t channelStrideBytes = 0;
  uint64_t audioInOffset = 0;
  uint64_t audioOutOffset = 0;
  uint64_t ringStdOffset = 0;
  uint64_t ringCtrlOffset = 0;
  uint64_t ringUiOffset = 0;
  uint64_t mailboxOffset = 0;
  std::atomic<uint64_t> uiVersion{0};
  uint64_t uiVisualSampleCount = 0;
  uint64_t uiGlobalNanotickPlayhead = 0;
  uint32_t uiTrackCount = 0;
  uint32_t reservedUi = 0;
  float uiTrackPeakRms[kUiMaxTracks]{};
};

struct alignas(64) RingHeader {
  uint32_t capacity = 0;
  uint32_t entrySize = 0;
  std::atomic<uint32_t> readIndex{0};
  std::atomic<uint32_t> writeIndex{0};
  uint32_t reserved[12]{};
};

struct alignas(64) EventEntry {
  uint64_t sampleTime = 0;
  uint32_t blockId = 0;
  uint16_t type = 0;
  uint16_t size = 0;
  uint32_t flags = 0;
  uint8_t payload[40]{};
};

enum class EventType : uint16_t {
  Midi = 1,
  Param = 2,
  Transport = 3,
  ReplayComplete = 4,
};

struct alignas(64) BlockMailbox {
  std::atomic<uint32_t> completedBlockId{0};
  std::atomic<uint64_t> completedSampleTime{0};
  std::atomic<uint64_t> replayAckSampleTime{0};
  uint32_t reserved[11]{};
};

size_t alignUp(size_t value, size_t alignment);
size_t channelStrideBytes(uint32_t blockSize);
size_t ringBytes(uint32_t capacity);
size_t sharedMemorySize(const ShmHeader& header,
                        uint32_t ringStdCapacity,
                        uint32_t ringCtrlCapacity,
                        uint32_t ringUiCapacity);

}  // namespace daw
