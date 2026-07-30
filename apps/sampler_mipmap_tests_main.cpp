// THE MIP-MAP, AND THE ONE MEASUREMENT THAT MATTERS: FOLD-BACK ENERGY.
//
// docs/SAMPLER_DESIGN.md S3 requires a negative control here, in those words, because this is the
// feature most likely to ship as a green suite that passes with the thing bypassed. Every
// structural test — "the levels exist", "they are half the length", "the right level is chosen" —
// passes perfectly against a mip-map that is built and then never read. So the assertion is
// acoustic: play a tone pitched up two octaves and measure the energy that lands where NO
// HARMONIC OF IT CAN BE.
//
// That is what aliasing IS. Reading faster than 1:1 folds source content above Nyquist/ratio back
// down into the band, and unlike most distortion it is INHARMONIC — it does not move with the
// note. A hi-hat pitched up two octaves acquires a fixed metallic ring that is nowhere in the
// source. A test that measured total distortion, or THD, or peak level would not see it.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "apps/sampler_voice.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
}

constexpr double kRate = 48000.0;

// THE FIXTURE IS TWO SINES, AND THE CHOICE IS THE WHOLE MEASUREMENT.
//
//   500 Hz   audible content, so both renders have signal and the comparison is not two silences
//   10 kHz   the component that FOLDS. Read at 4x it lands at 40 kHz, past Nyquist, so it
//            reflects to |48000 - 40000| = 8000 Hz.
//
// 8 kHz is the point: the played note is 500*4 = 2 kHz, and a pure SINE has no 4th harmonic. So
// any energy at 8 kHz is fold-back and nothing else — and the mip-map's level 2 is low-passed at
// 6 kHz before decimation, so it cannot contain the 10 kHz component at all.
//
// A first attempt used a harmonically rich tone and measured "energy halfway between harmonics".
// It read 2 dB — not because the mip-map was failing but because unwindowed Goertzel leakage from
// strong partials 440 Hz away swamped the bins being probed. The MEASUREMENT was the problem.
// One tone, one place it can fold to, and a window, rather than tuning a threshold until it
// passes.
std::vector<float> foldFixture(uint64_t frames) {
  std::vector<float> v(frames);
  for (uint64_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kRate;
    v[i] = static_cast<float>(0.35 * std::sin(2.0 * M_PI * 500.0 * t) +
                              0.35 * std::sin(2.0 * M_PI * 10000.0 * t));
  }
  return v;
}

// Energy at one frequency, HANN-WINDOWED. The window is not decoration: an unwindowed Goertzel's
// sidelobes decay so slowly that a strong partial hundreds of hertz away dominates the bin, which
// is exactly what made the first version of this test read 2 dB.
double energyAt(const std::vector<float>& x, double hz) {
  const int n = static_cast<int>(x.size());
  if (n < 16) {
    return 0.0;
  }
  const double k = 2.0 * std::cos(2.0 * M_PI * hz / kRate);
  double s1 = 0.0, s2 = 0.0;
  for (int i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));
    const double s0 = x[i] * w + k * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2);
}

// Renders one voice, mono, hard left so the pan law is unity.
std::vector<float> renderVoice(const daw::SamplerVoiceSpec& spec, uint32_t frames) {
  std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
  float* planes[2] = {l.data(), r.data()};
  daw::SamplerVoice v;
  v.start(spec, 1, 1, 0);
  v.render(planes, 2, 0, frames);
  return l;
}

}  // namespace

int main() {
  // ---- THE LEVELS ARE BUILT, HALVING EACH TIME.
  {
    std::vector<std::vector<float>> src(1, foldFixture(48000));
    const auto mips = daw::buildMipmap(src, 48000);
    check(mips.size() == daw::kMipLevels - 1, "four levels above the source are built");
    uint64_t expect = 24000;
    for (const auto& m : mips) {
      check(m.frames == expect, "each level is half the length of the one above");
      check(m.channels.size() == 1, "and keeps its channel count");
      expect /= 2;
    }
  }
  {
    // A SOURCE TOO SHORT TO DECIMATE stops early rather than inventing audio. A four-frame level
    // cannot feed a 4-tap interpolator, and padding would put samples in the file that are not.
    std::vector<std::vector<float>> src(1, std::vector<float>(20, 0.5f));
    const auto mips = daw::buildMipmap(src, 20);
    check(mips.size() < daw::kMipLevels - 1, "a very short source stops building levels early");
    for (const auto& m : mips) {
      check(m.frames >= 4, "and never produces a level too short to interpolate");
    }
  }
  {
    // AN EMPTY SOURCE is inert rather than a crash. Reachable from a failed decode.
    const auto mips = daw::buildMipmap({}, 0);
    check(mips.empty(), "an empty source builds no levels");
  }

  // ---- UNITY DC GAIN. Without it each level is quieter than the last and a note sweeping an
  // octave boundary STEPS IN LEVEL — which would be blamed on the crossfade for a long time.
  {
    std::vector<std::vector<float>> src(1, std::vector<float>(4096, 0.5f));  // DC
    const auto mips = daw::buildMipmap(src, 4096);
    check(!mips.empty(), "levels built from DC");
    for (size_t i = 0; i < mips.size(); ++i) {
      const auto& c = mips[i].channels[0];
      // Away from the clamped edges, DC must survive at its own level.
      const float mid = c[c.size() / 2];
      check(std::fabs(mid - 0.5f) < 0.005f,
            "each level preserves DC at unity — a level that is quieter than its predecessor "
            "makes an octave sweep step in LEVEL, which reads as a broken crossfade");
    }
  }

  // ---- LEVEL SELECTION. Below unity there is nothing to fix; each octave up steps one level.
  {
    check(daw::mipLevelFor(0.5, 4) == 0, "pitching DOWN uses level 0 — the mip-map is not for it");
    check(daw::mipLevelFor(1.0, 4) == 0, "unity uses level 0");
    check(daw::mipLevelFor(1.9, 4) == 0, "just under an octave still uses level 0");
    check(daw::mipLevelFor(2.0, 4) == 1, "an octave up steps to level 1");
    check(daw::mipLevelFor(4.0, 4) == 2, "two octaves, level 2");
    check(daw::mipLevelFor(16.0, 4) == 4, "four octaves, level 4");
    check(daw::mipLevelFor(64.0, 4) == 4, "and it CLAMPS rather than reading past the last level");
    check(daw::mipLevelFor(4.0, 1) == 1, "with only one level available it clamps to that");
    check(daw::mipLevelFor(4.0, 0) == 0, "with no levels it stays at 0 rather than indexing none");
  }

  // ---- THE BOUNDARY IS CROSSED, NOT STEPPED OVER. A static note at a boundary sounds correct on
  // either level; a SWEEP through one does not, and sweeps are what pitch envelopes do.
  {
    check(daw::mipBlend(2.0, 1) == 0.0f, "at the boundary the blend is entirely the lower level");
    const float mid = daw::mipBlend(2.0 * std::pow(2.0, 0.5 / 12.0), 1);
    check(mid > 0.3f && mid < 0.7f, "half a semitone above it is half way across");
    check(daw::mipBlend(2.0 * std::pow(2.0, 1.0 / 12.0), 1) == 1.0f,
          "a semitone above it is entirely the upper level");
    check(daw::mipBlend(3.9, 1) == 1.0f, "and it stays there for the rest of the octave");
    check(daw::mipBlend(1.5, 0) == 0.0f, "level 0 never blends — there is nothing below it");
  }

  // ---- THE MEASUREMENT. Pitched up TWO OCTAVES, with and without the mip-map, measured at the
  // ONE frequency the fold can land on. This is the negative control the design demands, run as
  // a test rather than as a one-off — and the bypassed path is `quality = Vintage`, a REAL
  // setting, so this simultaneously proves the mip-map works and that Vintage genuinely bypasses
  // it (a "quality" control that quietly did the same thing at every setting is its own defect).
  {
    const uint64_t frames = 48000;
    std::vector<std::vector<float>> src(1, foldFixture(frames));
    const auto mips = daw::buildMipmap(src, frames);
    std::vector<const float*> planes{src[0].data()};

    daw::SamplerVoiceSpec spec;
    spec.source.planes = planes.data();
    spec.source.channels = 1;
    spec.source.frames = frames;
    spec.source.sampleRate = kRate;
    spec.source.mips = mips.data();
    spec.source.mipCount = static_cast<uint32_t>(mips.size());
    spec.ratio = 4.0;  // two octaves up
    spec.pan = -1.0f;

    spec.quality = 1;  // Fast — uses the mip-map
    const std::vector<float> clean = renderVoice(spec, 8000);
    spec.quality = 0;  // Vintage — deliberately does NOT
    const std::vector<float> gritty = renderVoice(spec, 8000);

    double peakClean = 0.0, peakGritty = 0.0;
    for (uint32_t i = 0; i < clean.size(); ++i) {
      peakClean = std::max(peakClean, std::fabs(static_cast<double>(clean[i])));
      peakGritty = std::max(peakGritty, std::fabs(static_cast<double>(gritty[i])));
    }
    check(peakClean > 0.05 && peakGritty > 0.05,
          "both renders produced audible signal — comparing two silences would pass this check "
          "while proving nothing at all");

    const double aliasClean = energyAt(clean, 8000.0);
    const double aliasGritty = energyAt(gritty, 8000.0);
    const double db = 10.0 * std::log10((aliasGritty + 1e-30) / (aliasClean + 1e-30));
    std::printf("  fold-back at 8 kHz with the mip-map bypassed is %.1f dB above with it\n", db);
    check(db > 40.0,
          "the mip-map removes at least 40 dB of fold-back at +24 semitones. If this is near 0 "
          "the levels are being built and never read, which every structural test in this file "
          "would still pass");
  }

  // ---- AND IT DOES NOT DESTROY THE SIGNAL IT IS BAND-LIMITING. A mip-map that simply muted the
  // top four octaves would ace the measurement above.
  {
    const uint64_t frames = 48000;
    std::vector<std::vector<float>> src(1, foldFixture(frames));
    const auto mips = daw::buildMipmap(src, frames);
    std::vector<const float*> planes{src[0].data()};
    daw::SamplerVoiceSpec spec;
    spec.source.planes = planes.data();
    spec.source.channels = 1;
    spec.source.frames = frames;
    spec.source.sampleRate = kRate;
    spec.source.mips = mips.data();
    spec.source.mipCount = static_cast<uint32_t>(mips.size());
    spec.ratio = 4.0;
    spec.pan = -1.0f;
    spec.quality = 1;
    const std::vector<float> out = renderVoice(spec, 8000);
    const double wanted = energyAt(out, 2000.0);
    const double alias = energyAt(out, 8000.0);
    check(wanted > alias * 100.0,
          "the band-limited render KEEPS its wanted 2 kHz content while removing the fold — a "
          "mip-map that muted everything would score perfectly on the measurement above");
  }

  // ---- AT UNITY THE MIP-MAP IS NOT IN THE PATH AT ALL. Anti-aliasing that also filtered
  // untransposed playback would dull every drum in the kit for no reason.
  {
    const uint64_t frames = 4000;
    std::vector<std::vector<float>> src(1, std::vector<float>(frames));
    for (uint64_t i = 0; i < frames; ++i) {
      src[0][i] = static_cast<float>(i) / static_cast<float>(frames);
    }
    const auto mips = daw::buildMipmap(src, frames);
    std::vector<const float*> planes{src[0].data()};
    daw::SamplerVoiceSpec spec;
    spec.source.planes = planes.data();
    spec.source.channels = 1;
    spec.source.frames = frames;
    spec.source.sampleRate = kRate;
    spec.source.mips = mips.data();
    spec.source.mipCount = static_cast<uint32_t>(mips.size());
    spec.ratio = 1.0;
    spec.pan = -1.0f;
    spec.quality = 1;
    const std::vector<float> out = renderVoice(spec, 2000);
    bool exact = true;
    for (uint32_t i = 10; i < 1900; ++i) {
      if (std::fabs(out[i] - src[0][i]) > 2e-3f) {
        exact = false;
      }
    }
    check(exact, "at unity ratio the output IS the source — the mip-map must not be in the path, "
                 "or every untransposed drum in the kit is quietly filtered");
  }

  if (g_fail == 0) {
    std::printf("sampler_mipmap_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
