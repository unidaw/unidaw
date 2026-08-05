#pragma once

#include <cstdint>

namespace daw {

// THE PIPELINE DEPTH, AND THE ONE QUESTION IT ACTUALLY ANSWERS.
//
// The engine runs the out-of-process host a fixed number of blocks ahead of the audio device:
//
//     Block A: Writing    (engine, the future)
//     Block B: Processing (host)
//     Block C: Reading    (device, the present)
//
// so with numBlocks=3 the engine head is 2 blocks in front of what is being heard. That gap is
// real and it is what getLatencySamples() reports.
//
// WHAT THIS CLASS IS NOT. It once also mapped engine samples to a separate "plugin timeline" by
// subtracting that gap, and every event stamp AND the host's own block window went through that
// same subtraction — so the constant cancelled and the mapping did nothing. Its only surviving
// effect was the clamp below, applied where the subtraction would go negative, and there it was
// destructive: every event in the first latencySamples_ collapsed onto a block boundary. Measured
// with the Identity fixture, three notes written at samples 0, 400 and 900 rendered as TWO pulses,
// at 0 and 512, the first one CLIPPED at full scale because two notes had been summed onto the
// same sample. The same three notes at 5000/5400/5900 render as three exact pulses. The master
// host had never used the mapping at all — it passes its engine sample as both arguments — which
// is the other half of the proof that the offset was unobservable.
//
// So the plugin timeline IS the engine timeline, the host is given the engine's own sample clock,
// and the subtraction survives only where it was always the real answer: the visual playhead.
class LatencyManager {
 public:
  LatencyManager() = default;

  void init(uint32_t blockSize, uint32_t numBlocks) {
    blockSize_ = blockSize;
    latencySamples_ = (numBlocks > 0) ? (numBlocks - 1) * blockSize : 0;
  }

  uint64_t getLatencySamples() const { return latencySamples_; }

  // WHERE THE PLAYHEAD SHOULD BE DRAWN, given the engine head. The engine is latencySamples_ ahead
  // of the device, so what a listener is hearing right now was produced that long ago and the
  // cursor belongs there — otherwise it runs ahead of the sound by the whole pipeline depth.
  //
  // THE CLAMP IS CORRECT HERE and nowhere else. Before the pipeline has filled, nothing has been
  // heard yet, so the honest answer is the start of the song. This function returns one number per
  // block for one cursor, so a clamped region costs a stationary playhead for a few milliseconds;
  // it destroys no ordering, because there is no ordering in a single scalar to destroy. That is
  // exactly the difference from stamping N events with it, which is what used to happen.
  uint64_t visualPlayheadSample(uint64_t engineSampleStart) const {
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
