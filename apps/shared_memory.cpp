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

size_t ringBytes(uint32_t capacity) {
  const size_t header = alignUp(sizeof(RingHeader), 64);
  const size_t entries = static_cast<size_t>(capacity) * sizeof(EventEntry);
  return header + alignUp(entries, 64);
}

size_t sharedMemorySize(const ShmHeader& header,
                        uint32_t ringStdCapacity,
                        uint32_t ringCtrlCapacity) {
  size_t offset = alignUp(sizeof(ShmHeader), 64);
  const size_t stride = header.channelStrideBytes;
  const size_t inBlockBytes = static_cast<size_t>(header.numChannelsIn) * stride;
  const size_t outBlockBytes = static_cast<size_t>(header.numChannelsOut) * stride;
  offset += alignUp(inBlockBytes * header.numBlocks, 64);
  offset += alignUp(outBlockBytes * header.numBlocks, 64);
  offset += alignUp(ringBytes(ringStdCapacity), 64);
  offset += alignUp(ringBytes(ringCtrlCapacity), 64);
  offset += alignUp(sizeof(BlockMailbox), 64);
  return alignUp(offset, 64);
}

}  // namespace daw
