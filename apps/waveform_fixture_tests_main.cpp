// Stage 5: the waveform pyramid, verified against the real fixture WAVs the frontend
// designed to catch how peaks go wrong (presets/audio/waveform_probe*.wav). Decodes
// through the actual product path (decodeAudioFile builds the pyramid from the same channels it keeps)
// and asserts the section values — including the NEGATIVE direction the frontend asked
// for: an impulse must stay full-scale at a coarse level, which an averaging pyramid
// (the classic wrong implementation) cannot do. No audio device: pure file decode.
#include "platform_juce/juce_wrapper.h"
#include "apps/waveform_pyramid.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace daw;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

// The fixture's per-second sections (44100 frames each), from gen_audio_fixture.py:
//   0 silence · 1 full-scale · 2 +/-8192 · 3 near-full · 4 impulses · 5 DC +16384
//   6 full-scale · 7 silence
static constexpr uint64_t kSec = 44100;

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "../presets/audio";

  // ---- Mono ----
  const auto mono = decodeAudioFile(dir + "/waveform_probe.wav");
  CHECK(mono.ok);
  CHECK(mono.pyramid != nullptr);
  if (!mono.pyramid) {
    std::printf("cannot open %s/waveform_probe.wav\n", dir.c_str());
    return 1;
  }
  const auto& p = *mono.pyramid;
  CHECK(p.channels == 1);
  CHECK(p.frames == 8 * kSec);          // 352800
  CHECK(!p.clipped);
  CHECK(std::fabs(p.absPeak - 32767.0f / 32768.0f) < 1e-4f);  // 16-bit full scale
  CHECK(p.levelMask == 524225u);        // level 0 + decim 64,128,...,262144

  const uint32_t sel0[1] = {0};

  // Section 5 is DC +0.5: a 64-bucket wholly inside it is a solid 16384/16384 block
  // above zero (the mirrored-rendering catch). 220544 = 5*44100 rounded up to *64.
  {
    int16_t out[2] = {0, 0};
    const auto r = sliceWaveform(p, sel0, 1, 220544, 64, 1, out);
    CHECK(r.columns == 1 && out[0] == 16384 && out[1] == 16384);
  }

  // Section 2 is a quarter-scale (+/-8192) 440 Hz sine. Over a 256-frame bucket
  // (~2.5 periods) the min/max reach the sine's peaks, so a correct pyramid reports
  // near +/-8192 — quarter scale, distinct from both full scale and silence. 88320
  // = 256*345, inside [88200,132300).
  {
    int16_t out[2] = {0, 0};
    const auto r = sliceWaveform(p, sel0, 1, 88320, 256, 1, out);
    CHECK(r.columns == 1);
    CHECK(out[1] >= 8000 && out[1] <= 8192);
    CHECK(out[0] <= -8000 && out[0] >= -8192);
  }

  // The impulse at frame 176400 (start of section 4) survives to EVERY stored level:
  // the bucket covering it reports full-scale max at each decimation. This is the
  // assertion an averaging pyramid fails.
  const uint64_t impulse = 4 * kSec;  // 176400, an exact section boundary
  for (size_t k = 0; k < p.levelDecimations.size(); ++k) {
    const uint32_t d = p.levelDecimations[k];
    const uint64_t first = (impulse / d) * d;  // bucket-aligned
    int16_t out[2] = {0, 0};
    const auto r = sliceWaveform(p, sel0, 1, first, d, 1, out);
    CHECK(r.columns == 1 && out[1] == 32767);
  }

  // NEGATIVE DIRECTION (frontend's ask): at a coarse decimation the impulse bucket is
  // still full scale. An averaging implementation would return ~32767/4096 here, so
  // asserting > 0.9 full scale discriminates min/max from averaging in one line.
  {
    const uint32_t d = 4096;                 // a stored level (levelMask bit 12)
    const uint64_t first = (impulse / d) * d;
    int16_t out[2] = {0, 0};
    const auto r = sliceWaveform(p, sel0, 1, first, d, 1, out);
    CHECK(r.columns == 1 && out[1] > static_cast<int16_t>(0.9f * 32767.0f));
  }

  // ---- Stereo: L = -R ----
  const auto st = decodeAudioFile(dir + "/waveform_probe_stereo.wav");
  CHECK(st.ok && st.pyramid != nullptr);
  if (st.pyramid) {
    const auto& s = *st.pyramid;
    CHECK(s.channels == 2);
    CHECK(std::fabs(s.absPeak - 32767.0f / 32768.0f) < 1e-4f);  // NOT ~0: pre-downmix
    // A section-1 bucket: channel 1 is the exact negation of channel 0 (min/max
    // swapped and negated). A mono downmix would be silent here.
    const uint32_t sel01[2] = {0, 1};
    int16_t out[4] = {0, 0, 0, 0};
    const auto r = sliceWaveform(s, sel01, 2, kSec + 64, 64, 1, out);
    CHECK(r.columns == 1);
    CHECK(out[2] == -out[1] && out[3] == -out[0]);  // ch1 = negate(ch0)
    // And that bucket is genuinely loud (full scale), not two quiet channels.
    CHECK(out[1] > static_cast<int16_t>(0.9f * 32767.0f));
  }

  if (g_fail == 0) std::printf("waveform_fixture: all assertions passed\n");
  return g_fail == 0 ? 0 : 1;
}
