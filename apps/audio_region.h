#pragma once

#include <cstdint>

namespace daw {

// One placed audio region resolved for playback: where it sits on the output
// timeline (in output sample frames), where it reads from its source, and its
// gain + linear fades. All timeline quantities are in ENGINE output samples;
// convert from nanoticks at the call site so this stays a pure sample-domain DSP
// step (the audio analogue of placementEventsInWindow for notes).
struct AudioRegionParams {
  int64_t regionStartSample = 0;    // output frame where the region begins (at)
  int64_t regionLengthSamples = 0;  // the region's timeline extent, in out frames
  uint64_t sourceStartFrame = 0;    // in-point into the source, in source frames
  double sourceRate = 48000.0;      // the source file's sample rate
  double engineRate = 48000.0;      // the engine's output sample rate
  float gain = 1.0f;                // linear gain applied across the region
  int64_t fadeInSamples = 0;        // linear fade in over the first N out frames
  int64_t fadeOutSamples = 0;       // linear fade out over the last N out frames
};

// Renders `count` output frames starting at absolute output frame
// `blockStartSample` into `out` (mono, ADDED to whatever is there), reading a
// mono `source` of `sourceFrames` frames. The region plays from sourceStartFrame,
// resampled from sourceRate to engineRate by linear interpolation, with `gain` and
// linear fades. Frames outside the region span, or past the end of the source,
// contribute nothing. Pure and deterministic so the RT mixer and its unit test
// share one definition; no allocation, no locks — safe to call on the audio thread.
inline void renderAudioRegionBlock(const AudioRegionParams& p, const float* source,
                                   uint64_t sourceFrames, int64_t blockStartSample,
                                   int count, float* out) {
  if (!source || !out || count <= 0 || sourceFrames == 0) {
    return;
  }
  const double ratio = p.engineRate > 0.0 ? p.sourceRate / p.engineRate : 1.0;
  for (int i = 0; i < count; ++i) {
    const int64_t s = blockStartSample + static_cast<int64_t>(i);
    const int64_t rel = s - p.regionStartSample;  // frame within the region
    if (rel < 0 || rel >= p.regionLengthSamples) {
      continue;  // before the region starts / at or past its end
    }
    // Fractional read position in the source, resampled to the engine rate.
    const double srcPos =
        static_cast<double>(p.sourceStartFrame) + static_cast<double>(rel) * ratio;
    if (srcPos < 0.0) {
      continue;
    }
    const int64_t i0 = static_cast<int64_t>(srcPos);
    if (i0 < 0 || static_cast<uint64_t>(i0) >= sourceFrames) {
      continue;  // ran off the end of the source
    }
    const double frac = srcPos - static_cast<double>(i0);
    const float a = source[i0];
    const float b =
        (static_cast<uint64_t>(i0 + 1) < sourceFrames) ? source[i0 + 1] : a;
    float sample = static_cast<float>(static_cast<double>(a) +
                                      (static_cast<double>(b) - a) * frac);

    // Linear fades, expressed as an envelope multiplier on top of gain.
    float env = p.gain;
    if (p.fadeInSamples > 0 && rel < p.fadeInSamples) {
      env *= static_cast<float>(rel) / static_cast<float>(p.fadeInSamples);
    }
    if (p.fadeOutSamples > 0 &&
        rel >= p.regionLengthSamples - p.fadeOutSamples) {
      const int64_t remaining = p.regionLengthSamples - rel;  // frames left (>=1)
      env *= static_cast<float>(remaining) / static_cast<float>(p.fadeOutSamples);
    }
    out[i] += sample * env;
  }
}

}  // namespace daw
