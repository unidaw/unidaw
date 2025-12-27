#include "apps/audio_shm.h"

namespace daw {

float* audioOutChannelPtr(void* base,
                          const ShmHeader& header,
                          uint32_t blockIndex,
                          uint32_t channel) {
  const uint64_t block = static_cast<uint64_t>(blockIndex % header.numBlocks);
  const uint64_t stride = header.channelStrideBytes;
  const uint64_t blockBytes = static_cast<uint64_t>(header.numChannelsOut) * stride;
  const uint64_t offset = header.audioOutOffset + block * blockBytes +
                          static_cast<uint64_t>(channel) * stride;
  return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + offset);
}

float* audioInChannelPtr(void* base,
                         const ShmHeader& header,
                         uint32_t blockIndex,
                         uint32_t channel) {
  const uint64_t block = static_cast<uint64_t>(blockIndex % header.numBlocks);
  const uint64_t stride = header.channelStrideBytes;
  const uint64_t blockBytes = static_cast<uint64_t>(header.numChannelsIn) * stride;
  const uint64_t offset = header.audioInOffset + block * blockBytes +
                          static_cast<uint64_t>(channel) * stride;
  return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + offset);
}

}  // namespace daw
