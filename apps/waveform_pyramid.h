#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace daw {

// A per-source waveform pyramid held in engine memory (never in SHM): Q15 level-0
// samples plus min/max levels at decimation 64, 128, 256, ... one windowed slice of
// which answers a RequestWaveform. All values are in the SOURCE frame domain, anchored
// to frame 0, pre-gain and pre-fade. See the waveform data contract §1.
//
// Layout, all channel-planar (channel c is a contiguous run):
//   level0[c * frames + i]                         — the i16 Q15 sample
//   levelPairs[k][(c * bucketsForLevel[k] + b)*2]  — bucket b's min, and +1 its max
struct WaveformPyramid {
  uint32_t channels = 0;   // published channels = min(sourceChannels, 2)
  uint64_t frames = 0;     // frames per channel
  float absPeak = 0.0f;    // max|x| over the whole source, unclamped, pre-gain
  bool clipped = false;    // |x| > 1 seen anywhere (flag bit1)
  bool channelsTruncated = false;  // source had > 2 channels (flag bit0)

  std::vector<int16_t> level0;                     // channels * frames
  std::vector<uint32_t> levelDecimations;          // 64, 128, 256, ...
  std::vector<std::vector<int16_t>> levelPairs;    // one buffer per decimation level
  uint32_t levelMask = 0;  // bit k set => a level at decimation (1<<k) exists

  // Buckets per channel at levelPairs[k] (== ceil(frames / levelDecimations[k])).
  uint32_t bucketsAtLevel(size_t k) const {
    const uint32_t d = levelDecimations[k];
    return static_cast<uint32_t>((frames + d - 1) / d);
  }
};

// The Q15 convention, stated once in the contract §1.3: quantise against 32768 (not
// 32767) so 16-bit PCM read as x/32768 round-trips exactly, then clamp to [-32767,
// +32767] (−32768 would be the only value to saturate, at digital full-scale negative).
inline int16_t quantizeQ15(float x) {
  const long v = std::lround(static_cast<double>(x) * 32768.0);
  return static_cast<int16_t>(v < -32767L ? -32767L : (v > 32767L ? 32767L : v));
}

// Build the pyramid from planar float channels (source order). Publishes at most 2
// channels but scans all of them for absPeak/clipped so the flags are honest.
// kWaveformBaseDecim must be a power of two (64 in the contract).
inline WaveformPyramid buildWaveformPyramid(const float* const* channels,
                                            uint32_t sourceChannels,
                                            uint64_t frames,
                                            uint32_t baseDecim = 64) {
  WaveformPyramid p;
  p.frames = frames;
  p.channelsTruncated = sourceChannels > 2;
  p.channels = std::min<uint32_t>(sourceChannels, 2);
  if (p.channels == 0 || frames == 0) {
    p.levelMask = 1u;  // level 0 (empty) still "exists"
    return p;
  }

  // Level 0: Q15, planar. absPeak/clipped scan ALL source channels (a mixdown is
  // false — an out-of-phase pair sums to silence — so the flags describe the file).
  p.level0.resize(static_cast<size_t>(p.channels) * static_cast<size_t>(frames));
  double absPeak = 0.0;
  bool clipped = false;
  for (uint32_t c = 0; c < sourceChannels; ++c) {
    const float* src = channels[c];
    const bool publish = c < p.channels;
    int16_t* dst = publish ? &p.level0[static_cast<size_t>(c) * frames] : nullptr;
    for (uint64_t i = 0; i < frames; ++i) {
      const float x = src[i];
      const double a = std::fabs(static_cast<double>(x));
      if (a > absPeak) absPeak = a;
      if (a > 1.0) clipped = true;
      if (publish) dst[i] = quantizeQ15(x);
    }
  }
  p.absPeak = static_cast<float>(absPeak);
  p.clipped = clipped;
  p.levelMask = 1u;  // bit 0: level 0 (decimation 1) exists

  // Level `baseDecim` reduces level 0 directly; higher levels reduce pairwise.
  // min/max is associative + idempotent, so a pairwise level is bit-identical to one
  // built straight from level 0 (the contract makes this a test).
  uint32_t decim = baseDecim;
  const std::vector<int16_t>* prev = nullptr;   // previous level's pairs
  uint32_t prevBuckets = 0;
  while (decim <= frames || p.levelPairs.empty()) {
    const uint32_t buckets =
        static_cast<uint32_t>((frames + decim - 1) / decim);
    std::vector<int16_t> pairs(static_cast<size_t>(p.channels) * buckets * 2);
    for (uint32_t c = 0; c < p.channels; ++c) {
      for (uint32_t b = 0; b < buckets; ++b) {
        int16_t lo = INT16_MAX;
        int16_t hi = INT16_MIN;
        if (prev == nullptr) {
          // Reduce level 0: [b*decim, (b+1)*decim) source frames.
          const int16_t* s = &p.level0[static_cast<size_t>(c) * frames];
          const uint64_t start = static_cast<uint64_t>(b) * decim;
          const uint64_t end = std::min<uint64_t>(start + decim, frames);
          for (uint64_t i = start; i < end; ++i) {
            lo = std::min(lo, s[i]);
            hi = std::max(hi, s[i]);
          }
        } else {
          // Reduce the previous level pairwise: two child buckets per parent.
          const int16_t* cp = &(*prev)[static_cast<size_t>(c) * prevBuckets * 2];
          for (uint32_t ch = 0; ch < 2; ++ch) {
            const uint32_t cb = b * 2 + ch;
            if (cb >= prevBuckets) break;
            lo = std::min(lo, cp[cb * 2]);
            hi = std::max(hi, cp[cb * 2 + 1]);
          }
        }
        const size_t idx = (static_cast<size_t>(c) * buckets + b) * 2;
        pairs[idx] = lo;
        pairs[idx + 1] = hi;
      }
    }
    p.levelDecimations.push_back(decim);
    p.levelPairs.push_back(std::move(pairs));
    // levelMask bit for this decimation (log2, since decim is a power of two).
    uint32_t bit = 0;
    for (uint32_t d = decim; d > 1; d >>= 1) ++bit;
    p.levelMask |= (1u << bit);

    prev = &p.levelPairs.back();
    prevBuckets = buckets;
    if (buckets <= 1) break;   // top of the pyramid
    decim <<= 1;
  }
  return p;
}

// The outcome of slicing one window out of a pyramid.
struct WaveformSlice {
  uint32_t columns = 0;      // columns actually written per channel (<= requested)
  uint64_t frameCount = 0;   // columns * decimation, clipped at end of source
  bool truncated = false;    // fewer columns than requested (window ran past EOF)
  bool pastEof = false;      // requested window extended past the source end
};

// Answer one windowed RequestWaveform by filling `out` with channel-planar min/max
// pairs: for output channel oc, column i, out[(oc*columns + i)*2] is the min and +1
// the max, where `columns` is the returned count (NOT the requested one). `selChannels`
// lists the pyramid channel index for each of `outChannels` output planes (so a
// channelMask selecting only ch1 writes ch1's data at plane 0). `out` must hold
// outChannels * reqColumns * 2 int16s. Three regimes, one output shape (contract §3):
//   decimation == 1        -> each bucket is one sample, min == max (degenerate pair)
//   stored level present    -> slice it directly (the fast path; frame-0 anchoring makes
//                              firstFrame/decimation an exact bucket index)
//   otherwise               -> scan level 0 over decimation frames per bucket
// The scan and the slice are bit-identical for a stored decimation (min/max is
// associative + idempotent), which the tests assert.
inline WaveformSlice sliceWaveform(const WaveformPyramid& p,
                                   const uint32_t* selChannels,
                                   uint32_t outChannels, uint64_t firstFrame,
                                   uint32_t decimation, uint32_t reqColumns,
                                   int16_t* out) {
  WaveformSlice r;
  if (decimation == 0 || reqColumns == 0 || outChannels == 0) return r;

  // Columns available from firstFrame to the end of the source.
  uint32_t cols = 0;
  if (firstFrame < p.frames) {
    const uint64_t avail = p.frames - firstFrame;
    const uint64_t availCols = (avail + decimation - 1) / decimation;
    cols = static_cast<uint32_t>(std::min<uint64_t>(reqColumns, availCols));
  }
  r.columns = cols;
  r.frameCount =
      std::min<uint64_t>(static_cast<uint64_t>(cols) * decimation,
                         firstFrame < p.frames ? p.frames - firstFrame : 0);
  r.truncated = cols < reqColumns;
  r.pastEof = firstFrame + static_cast<uint64_t>(reqColumns) * decimation > p.frames;
  if (cols == 0) return r;

  // Find a stored level at exactly this decimation, if any.
  int level = -1;
  for (size_t k = 0; k < p.levelDecimations.size(); ++k) {
    if (p.levelDecimations[k] == decimation) {
      level = static_cast<int>(k);
      break;
    }
  }

  for (uint32_t oc = 0; oc < outChannels; ++oc) {
    const uint32_t sc = selChannels[oc];
    int16_t* dst = out + static_cast<size_t>(oc) * cols * 2;
    if (decimation == 1) {
      const int16_t* s0 = &p.level0[static_cast<size_t>(sc) * p.frames];
      for (uint32_t i = 0; i < cols; ++i) {
        const int16_t v = s0[firstFrame + i];
        dst[i * 2] = v;
        dst[i * 2 + 1] = v;
      }
    } else if (level >= 0) {
      const uint32_t buckets = p.bucketsAtLevel(static_cast<size_t>(level));
      const int16_t* lp =
          &p.levelPairs[level][static_cast<size_t>(sc) * buckets * 2];
      const uint64_t base = firstFrame / decimation;  // exact: frame-0 anchored
      for (uint32_t i = 0; i < cols; ++i) {
        const uint64_t b = base + i;
        if (b < buckets) {
          dst[i * 2] = lp[b * 2];
          dst[i * 2 + 1] = lp[b * 2 + 1];
        } else {
          dst[i * 2] = 0;
          dst[i * 2 + 1] = 0;
        }
      }
    } else {
      const int16_t* s0 = &p.level0[static_cast<size_t>(sc) * p.frames];
      for (uint32_t i = 0; i < cols; ++i) {
        const uint64_t start = firstFrame + static_cast<uint64_t>(i) * decimation;
        const uint64_t end = std::min<uint64_t>(start + decimation, p.frames);
        int16_t lo = INT16_MAX, hi = INT16_MIN;
        for (uint64_t f = start; f < end; ++f) {
          lo = std::min(lo, s0[f]);
          hi = std::max(hi, s0[f]);
        }
        dst[i * 2] = lo;
        dst[i * 2 + 1] = hi;
      }
    }
  }
  return r;
}

}  // namespace daw
