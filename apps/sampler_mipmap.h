#pragma once

// THE OCTAVE MIP-MAP: what makes pitching UP not sound cheap.
//
// Interpolation quality fixes pitching DOWN. It does nothing whatsoever for pitching UP, and
// conflating those two is the single most common sampler mistake — you can spend a 16-tap sinc on
// a slot transposed +24 and it will still sound like a bag of gravel, because the problem is not
// the interpolator.
//
// Reading a sample faster than 1:1 folds every source component above Nyquist/ratio back down
// into the audible band as ALIASING, and unlike most distortion it is INHARMONIC: it does not
// move with the note. A hi-hat pitched up two octaves acquires a fixed metallic ring that is
// nowhere in the source. The fix has to happen BEFORE the decimation, which means it has to be
// precomputed — you cannot filter your way out of it afterwards.
//
// So: band-limited copies at 1/2, 1/4, 1/8, 1/16 rate, each made from its predecessor through a
// halfband FIR. Reading level L at residual ratio ratio/2^L keeps the read rate in [1,2) where
// the fold-back is bounded by the filter's stopband instead of by luck.
//
// TWO THINGS THIS IS NOT:
//
//   * It is NOT WaveformPyramid (apps/waveform_pyramid.h). That one is min/max + Q15 for DRAWING
//     and is wrong for playback in every respect — it is not band-limited, it is not linear, and
//     its levels are chosen for pixels. Two pyramids, two purposes, and they must never share a
//     struct or someone will eventually draw with one and play the other.
//   * It is NOT free. Memory is +100% worst case (1/2 + 1/4 + 1/8 + ... -> 1), which is the right
//     trade for a sampler and would not be for a streaming audio clip.
//
// Built off the audio thread at intern time, beside the display pyramid.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace daw {

// Four levels above the original: 1/2 down to 1/16, which covers +48 semitones. Past that the
// residual ratio grows again and the aliasing returns, but a slot transposed more than four
// octaves is not a pitch decision, and pretending otherwise would cost memory for nothing.
inline constexpr uint32_t kMipLevels = 5;  // level 0 (the source) plus four

// A decimated copy. `frames` is this level's own length, NOT the source's.
struct MipLevel {
  std::vector<std::vector<float>> channels;  // planar
  std::vector<const float*> planes;          // pointers into `channels`, built once
  uint64_t frames = 0;

  void buildPlanes() {
    planes.clear();
    planes.reserve(channels.size());
    for (const auto& c : channels) {
      planes.push_back(c.data());
    }
  }
};

// Modified Bessel I0, by its series. Converges fast for the arguments a Kaiser window uses, and
// writing it out avoids depending on a platform's cyl_bessel_i.
inline double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= (x * 0.5) / static_cast<double>(k);
    const double t2 = term * term;
    sum += t2;
    if (t2 < 1e-18 * sum) {
      break;
    }
  }
  return sum;
}

// A 63-tap halfband low-pass at 0.25 of the sample rate — exactly the band that survives
// decimation by two — windowed with Kaiser beta = 8 for roughly 90 dB of stopband.
//
// HALFBAND means every even-indexed tap away from the centre is ZERO by construction. They are
// computed and left in anyway: zeroing them by hand would be an optimisation that changes nothing
// numerically and adds a way to get the indexing wrong.
inline std::vector<double> halfbandKernel() {
  constexpr int kTaps = 63;
  constexpr int kHalf = kTaps / 2;
  const double beta = 8.0;
  const double denom = besselI0(beta);
  std::vector<double> h(kTaps, 0.0);
  double sum = 0.0;
  for (int i = 0; i < kTaps; ++i) {
    const int n = i - kHalf;
    // sinc at cutoff 0.25 * fs -> normalised 0.5
    const double x = static_cast<double>(n) * 0.5;
    const double sinc = (n == 0) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
    const double r = static_cast<double>(n) / static_cast<double>(kHalf);
    const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom;
    h[i] = sinc * w * 0.5;
    sum += h[i];
  }
  // Normalise to unity DC gain. Without this each level is quieter than the last and a note
  // sweeping through an octave boundary steps in level — which would be blamed on the crossfade.
  if (sum > 0.0) {
    for (auto& v : h) {
      v /= sum;
    }
  }
  return h;
}

// Builds levels 1..(kMipLevels-1) from a planar level-0 source. Off the audio thread.
inline std::vector<MipLevel> buildMipmap(const std::vector<std::vector<float>>& source,
                                         uint64_t frames) {
  std::vector<MipLevel> levels;
  if (source.empty() || frames == 0) {
    return levels;
  }
  static const std::vector<double> h = halfbandKernel();
  const int taps = static_cast<int>(h.size());
  const int half = taps / 2;

  const std::vector<std::vector<float>>* prev = &source;
  uint64_t prevFrames = frames;
  for (uint32_t lvl = 1; lvl < kMipLevels; ++lvl) {
    const uint64_t outFrames = prevFrames / 2;
    if (outFrames < 4) {
      // Below four frames a level cannot feed a 4-tap interpolator, and a sample that short
      // cannot be transposed far enough to need one. Stopping is honest; padding would invent
      // audio that is not in the file.
      break;
    }
    MipLevel out;
    out.frames = outFrames;
    out.channels.resize(prev->size());
    for (size_t ch = 0; ch < prev->size(); ++ch) {
      const std::vector<float>& in = (*prev)[ch];
      std::vector<float>& dst = out.channels[ch];
      dst.assign(outFrames, 0.0f);
      for (uint64_t i = 0; i < outFrames; ++i) {
        const int64_t centre = static_cast<int64_t>(i) * 2;
        double acc = 0.0;
        for (int k = 0; k < taps; ++k) {
          int64_t idx = centre + (k - half);
          // CLAMPED at the edges, not wrapped: a sample's start and end are real boundaries, and
          // wrapping would fold the tail onto the head — which for a drum is a pre-echo.
          idx = std::clamp<int64_t>(idx, 0, static_cast<int64_t>(prevFrames) - 1);
          acc += h[k] * static_cast<double>(in[static_cast<size_t>(idx)]);
        }
        dst[i] = static_cast<float>(acc);
      }
    }
    out.buildPlanes();
    levels.push_back(std::move(out));
    prev = &levels.back().channels;
    prevFrames = outFrames;
  }
  return levels;
}

// Which level to read for a given playback ratio, and the residual ratio to read it at.
//
// LEVEL SWITCHING IS FREE BECAUSE POSITION IS CANONICAL IN LEVEL-0 FRAMES: the level-L read index
// is `pos >> L`, exact by construction. A pitch envelope or a glide sweeping through an octave
// boundary therefore does not jump — which is exactly why the voice holds 32.32 fixed point in
// level-0 frames rather than a float position per level.
inline uint32_t mipLevelFor(double ratio, uint32_t available) {
  if (!(ratio > 1.0) || available == 0) {
    return 0;
  }
  const int l = static_cast<int>(std::floor(std::log2(ratio)));
  return static_cast<uint32_t>(std::clamp<int>(l, 0, static_cast<int>(available)));
}

// How much of level L+1 to blend in, 0..1, over the semitone above the boundary.
//
// A STATIC note at a boundary would be fine without this — either level sounds correct. A SWEEP
// through one is not, and sweeps are what pitch envelopes and glides do, so the seam has to be
// crossed rather than stepped over.
inline float mipBlend(double ratio, uint32_t level) {
  if (level == 0) {
    return 0.0f;
  }
  const double lo = std::pow(2.0, static_cast<double>(level));
  const double hi = lo * std::pow(2.0, 1.0 / 12.0);  // one semitone
  if (ratio <= lo) {
    return 0.0f;
  }
  if (ratio >= hi) {
    return 1.0f;
  }
  return static_cast<float>((ratio - lo) / (hi - lo));
}

}  // namespace daw
