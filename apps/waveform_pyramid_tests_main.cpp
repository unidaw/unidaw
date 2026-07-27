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

  // --- sliceWaveform: the three regimes answer one window with one output shape ---
  {
    const uint32_t sel0[1] = {0};
    std::vector<int16_t> out(64 * 2, 0);

    // decimation 1 is degenerate: each column is one sample, min == max.
    auto r1 = sliceWaveform(p, sel0, 1, /*firstFrame*/ 128, /*decim*/ 1,
                            /*cols*/ 16, out.data());
    CHECK(r1.columns == 16 && !r1.truncated);
    for (uint32_t i = 0; i < 16; ++i) {
      const int16_t s = p.level0[128 + i];
      CHECK(out[i * 2] == s && out[i * 2 + 1] == s);
    }

    // A stored decimation (64) slices the level directly and equals a fresh scan of
    // level 0 — the fast path and the slow path are bit-identical.
    std::vector<int16_t> slice(8 * 2, 0), scan(8 * 2, 0);
    auto rs = sliceWaveform(p, sel0, 1, 0, 64, 8, slice.data());
    CHECK(rs.columns == 8);
    for (uint32_t b = 0; b < 8; ++b) {
      int16_t lo = INT16_MAX, hi = INT16_MIN;
      for (uint32_t f = b * 64; f < (b + 1) * 64; ++f) {
        lo = std::min(lo, p.level0[f]);
        hi = std::max(hi, p.level0[f]);
      }
      scan[b * 2] = lo;
      scan[b * 2 + 1] = hi;
    }
    CHECK(slice == scan);

    // A non-stored decimation (8) scans level 0; check the alternating-±1 region.
    auto r8 = sliceWaveform(p, sel0, 1, 256, 8, 4, out.data());
    CHECK(r8.columns == 4);
    for (uint32_t i = 0; i < 4; ++i) {
      CHECK(out[i * 2] == -32767 && out[i * 2 + 1] == 32767);
    }

    // A window offset by whole buckets picks the right buckets of the stored level.
    auto rw = sliceWaveform(p, sel0, 1, 128, 64, 2, out.data());
    const auto& L = p.levelPairs[0];  // decim-64 level
    CHECK(rw.columns == 2);
    CHECK(out[0] == L[(128 / 64) * 2] && out[1] == L[(128 / 64) * 2 + 1]);
    CHECK(out[2] == L[(192 / 64) * 2] && out[3] == L[(192 / 64) * 2 + 1]);

    // Past EOF: 8 columns of 64 from frame 256 in a 512-frame source -> 4 available.
    auto re = sliceWaveform(p, sel0, 1, 256, 64, 8, out.data());
    CHECK(re.columns == 4 && re.truncated && re.pastEof);
    CHECK(re.frameCount == 256);

    // firstFrame at/after EOF returns nothing but flags the overrun.
    auto rz = sliceWaveform(p, sel0, 1, 512, 64, 4, out.data());
    CHECK(rz.columns == 0 && rz.pastEof);
  }

  // Channel selection: on a stereo pyramid, selecting only ch1 writes ch1's data at
  // output plane 0.
  {
    std::vector<float> l(64), r(64);
    for (uint64_t i = 0; i < 64; ++i) {
      l[i] = (i % 2 == 0) ? 1.0f : -1.0f;
      r[i] = -l[i];
    }
    const float* st[2] = {l.data(), r.data()};
    const auto sp = buildWaveformPyramid(st, 2, 64, 64);
    const uint32_t sel1[1] = {1};
    int16_t out1[2] = {0, 0};
    auto rc = sliceWaveform(sp, sel1, 1, 0, 64, 1, out1);
    CHECK(rc.columns == 1);
    const uint32_t bkt = sp.bucketsAtLevel(0);
    const auto& LP = sp.levelPairs[0];
    CHECK(out1[0] == LP[(1 * bkt) * 2] && out1[1] == LP[(1 * bkt) * 2 + 1]);
  }

  if (g_fail == 0) std::printf("waveform_pyramid: all assertions passed\n");
  return g_fail == 0 ? 0 : 1;
}
