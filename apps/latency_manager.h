#pragma once

#include <cstdint>

namespace daw {

class LatencyManager {
 public:
  LatencyManager() = default;

  void init(uint32_t blockSize, uint32_t numBlocks) {
    blockSize_ = blockSize;
    // According to specs: globalLatencySamples = (numBlocks - 1) * blockSize
    // This accounts for the pipeline depth required for the out-of-process host.
    // If numBlocks=3, latency=2 blocks.
    // Block A: Writing (Future)
    // Block B: Processing (Host)
    // Block C: Reading (Present/Output)
    latencySamples_ = (numBlocks > 0) ? (numBlocks - 1) * blockSize : 0;
  }

  uint64_t getLatencySamples() const { return latencySamples_; }

  // Calculates the timestamp the Plugin should see (Plugin Time)
  // given the current Engine Head timestamp (Engine Time).
  // This compensates for the system latency so that audio generated for
  // "Musical Time X" aligns with the Audio Output at "Wall Clock X + Latency".
  // Wait, if we subtract latency, we are saying:
  // "Plugin, please generate audio for Time T - Latency".
  // We play it at T.
  // So we hear (T - Latency).
  // So the Visual Playhead (T - Latency) matches the Audio (T - Latency).
  uint64_t getCompensatedStart(uint64_t engineSampleStart) const {
    if (engineSampleStart >= latencySamples_) {
      return engineSampleStart - latencySamples_;
    }
    return 0; 
  }

 private:
  uint32_t blockSize_ = 0;
  uint64_t latencySamples_ = 0;
};

}  // namespace daw
