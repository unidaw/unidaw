#include "apps/shared_memory.h"

#include <cstddef>

namespace daw {

size_t alignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

size_t channelStrideBytes(uint32_t blockSize) {
  const size_t bytes = static_cast<size_t>(blockSize) * sizeof(float);
  return alignUp(bytes, 64);
}

size_t ringBytesForEntrySize(uint32_t capacity, size_t entrySize) {
  const size_t header = alignUp(sizeof(RingHeader), 64);
  const size_t entries = static_cast<size_t>(capacity) * entrySize;
  return header + alignUp(entries, 64);
}

size_t ringBytes(uint32_t capacity) {
  return ringBytesForEntrySize(capacity, sizeof(EventEntry));
}

size_t sharedMemorySize(const ShmHeader& header,
                        uint32_t ringStdCapacity,
                        uint32_t ringCtrlCapacity,
                        uint32_t ringUiCapacity,
                        uint32_t numAuxChannelsOut) {
  size_t offset = alignUp(sizeof(ShmHeader), 64);
  const size_t stride = header.channelStrideBytes;
  const size_t inBlockBytes = static_cast<size_t>(header.numChannelsIn) * stride;
  const size_t outBlockBytes = static_cast<size_t>(header.numChannelsOut) * stride;
  // Movement 4: the aux OUTPUT plane sits immediately after the main output plane, so
  // its offset is derivable from audioOutOffset (see auxOutputPlaneOffset). 0 aux
  // channels contributes 0 bytes, keeping the pre-multi-out layout byte-identical.
  const size_t auxBlockBytes = static_cast<size_t>(numAuxChannelsOut) * stride;
  offset += alignUp(inBlockBytes * header.numBlocks, 64);
  offset += alignUp(outBlockBytes * header.numBlocks, 64);
  offset += alignUp(auxBlockBytes * header.numBlocks, 64);
  offset += alignUp(ringBytes(ringStdCapacity), 64);
  offset += alignUp(ringBytes(ringCtrlCapacity), 64);
  offset += alignUp(ringBytes(ringUiCapacity), 64);
  offset += alignUp(sizeof(BlockMailbox), 64);
  return alignUp(offset, 64);
}

size_t auxOutputPlaneOffset(const ShmHeader& header) {
  const size_t stride = header.channelStrideBytes;
  const size_t outBlockBytes = static_cast<size_t>(header.numChannelsOut) * stride;
  return static_cast<size_t>(header.audioOutOffset) +
         alignUp(outBlockBytes * header.numBlocks, 64);
}

}  // namespace daw
