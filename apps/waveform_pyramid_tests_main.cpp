// Numeric tests for the waveform pyramid — the assertions the frontend's contract
// makes: Q15 exactness, the min/max reduce rule, bit-identity of pairwise vs direct
// levels, and that a transient survives to every zoom level. Header-only, no deps.
#include "apps/waveform_pyramid.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace daw;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

int main() {
  // Q15 convention (contract §1.3): quantise against 32768, clamp to [-32767, 32767].
  CHECK(quantizeQ15(0.0f) == 0);
  CHECK(quantizeQ15(0.5f) == 16384);
  CHECK(quantizeQ15(1.0f) == 32767);     // 32768 clamps to 32767
  CHECK(quantizeQ15(-1.0f) == -32767);
  CHECK(quantizeQ15(2.0f) == 32767);     // saturates

  // A signal with four one-eighth sections: silence, DC +0.5, alternating +/-1, and
  // an impulse at frame 400. 512 frames so the pyramid reaches a single top bucket.
  const uint64_t frames = 512;
  std::vector<float> ch(frames);
  for (uint64_t i = 0; i < frames; ++i) {
    if (i < 128) ch[i] = 0.0f;
    else if (i < 256) ch[i] = 0.5f;
    else if (i < 384) ch[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    else ch[i] = (i == 400) ? 1.0f : 0.0f;
  }
  const float* chans[1] = {ch.data()};
  const auto p = buildWaveformPyramid(chans, 1, frames, 64);

  CHECK(p.channels == 1);
  CHECK(p.frames == frames);
  CHECK(p.absPeak == 1.0f);
  CHECK(!p.clipped);
  // levels at 64,128,256,512 -> mask bits 0 (level0),6,7,8,9 = 0x3c1
  CHECK(p.levelMask == 0x3c1u);
  CHECK(p.levelDecimations.size() == 4);

  // DC +0.5 section: a 64-bucket fully inside [128,256) is a solid block at +16384
  // (min == max, above zero — catches mirrored rendering).
  {
    const auto& L = p.levelPairs[0];
    const uint32_t b = 192 / 64;  // covers [192,256), all DC
    CHECK(L[b * 2] == 16384 && L[b * 2 + 1] == 16384);
  }
  // Alternating +/-1: min and max both full-scale (catches one-magnitude buckets).
  {
    const auto& L = p.levelPairs[0];
    const uint32_t b = 320 / 64;
    CHECK(L[b * 2] == -32767 && L[b * 2 + 1] == 32767);
  }
  // Impulse: every level's bucket containing frame 400 reports full-scale max — a
  // transient must never vanish on zoom-out.
  for (size_t k = 0; k < p.levelPairs.size(); ++k) {
    const uint32_t d = p.levelDecimations[k];
    const uint32_t b = 400 / d;
    CHECK(p.levelPairs[k][b * 2 + 1] == 32767);
  }

  // Bit-identity: level k built pairwise == level k computed directly from level 0.
  for (size_t k = 1; k < p.levelPairs.size(); ++k) {
    const uint32_t d = p.levelDecimations[k];
    const uint32_t buckets = p.bucketsAtLevel(k);
    for (uint32_t b = 0; b < buckets; ++b) {
      int16_t lo = INT16_MAX, hi = INT16_MIN;
      const uint64_t s = static_cast<uint64_t>(b) * d;
      const uint64_t e = std::min<uint64_t>(s + d, frames);
      for (uint64_t i = s; i < e; ++i) {
        lo = std::min(lo, p.level0[i]);
        hi = std::max(hi, p.level0[i]);
      }
      CHECK(p.levelPairs[k][b * 2] == lo && p.levelPairs[k][b * 2 + 1] == hi);
    }
  }

  // Stereo L = -R: channel 1 is the exact negation of channel 0 at every level, and
  // the union envelope is full-scale where a mono downmix would be silent.
  {
    std::vector<float> l(64), r(64);
    for (uint64_t i = 0; i < 64; ++i) {
      l[i] = (i % 2 == 0) ? 1.0f : -1.0f;
      r[i] = -l[i];
    }
    const float* st[2] = {l.data(), r.data()};
    const auto s = buildWaveformPyramid(st, 2, 64, 64);
    CHECK(s.channels == 2);
    const auto& L = s.levelPairs[0];  // one bucket per channel
    const uint32_t bkt = s.bucketsAtLevel(0);
    // ch0 [min,max] == negation of ch1 [max,min]
    CHECK(L[0] == -L[bkt * 2 + 1] && L[1] == -L[bkt * 2]);
    // union envelope full-scale (a downmix would be flat/silent here)
    const int16_t umin = std::min(L[0], L[bkt * 2]);
    const int16_t umax = std::max(L[1], L[bkt * 2 + 1]);
    CHECK(umin == -32767 && umax == 32767);
  }

  if (g_fail == 0) std::printf("waveform_pyramid: all assertions passed\n");
  return g_fail == 0 ? 0 : 1;
}
