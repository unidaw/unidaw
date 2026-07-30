// THE VOICE. The headline property here is BLOCK-SIZE INVARIANCE — one note rendered at 64, 256
// and 1024 frames must produce BIT-IDENTICAL output.
//
// That is not a nice-to-have. It is the property that fails the moment a voice starts on a block
// boundary instead of on its own sample, so it is the check that keeps sample-accurate starts
// honest (docs/SAMPLER_DESIGN.md §3.5). Most shipping DAWs do not clear it. Retrofitting it after
// the rest of the sampler is built would be a rewrite, which is why it is asserted in S1.
//
// Everything here runs against float buffers with no engine, no device and no audio device.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

void checkNear(float got, float want, float tol, const char* what) {
  if (!(std::fabs(got - want) <= tol)) {
    std::printf("FAIL %s: got %.6f want %.6f (tol %.6f)\n", what, got, want, tol);
    ++g_fail;
  }
}

// A source whose every sample is DISTINCT and predictable, so a wrong read position is visible
// rather than plausible. A sine would alias against itself under varispeed and hide an off-by-one.
struct Fixture {
  std::vector<float> l, r;
  const float* planes[2]{};
  daw::SamplerSourceView view(uint32_t channels) {
    daw::SamplerSourceView v;
    planes[0] = l.data();
    planes[1] = r.data();
    v.planes = planes;
    v.channels = channels;
    v.frames = l.size();
    v.sampleRate = 48000.0;
    return v;
  }
};

Fixture ramp(uint64_t frames) {
  Fixture f;
  f.l.resize(frames);
  f.r.resize(frames);
  for (uint64_t i = 0; i < frames; ++i) {
    f.l[i] = static_cast<float>(i) / static_cast<float>(frames);   // 0 -> 1
    f.r[i] = -static_cast<float>(i) / static_cast<float>(frames);  // 0 -> -1, clearly different
  }
  return f;
}

// Renders one voice over `total` frames in chunks of `block`, into a flat stereo pair.
void renderChunked(const daw::SamplerVoiceSpec& spec,
                   uint32_t total,
                   uint32_t block,
                   uint32_t startOffset,
                   std::vector<float>& outL,
                   std::vector<float>& outR) {
  outL.assign(total, 0.0f);
  outR.assign(total, 0.0f);
  daw::SamplerVoice v;
  v.start(spec, 1, 1, 0);
  uint32_t done = 0;
  bool started = false;
  while (done < total) {
    const uint32_t n = std::min(block, total - done);
    float* planes[2] = {outL.data() + done, outR.data() + done};
    if (!started) {
      // The note begins at absolute sample `startOffset`. Whichever block that falls in, the
      // voice starts at its offset WITHIN that block — which is the whole point.
      if (done + n > startOffset) {
        const uint32_t within = startOffset - done;
        v.render(planes, 2, within, n - within);
        started = true;
      }
    } else {
      v.render(planes, 2, 0, n);
    }
    done += n;
  }
}

}  // namespace

int main() {
  // ---- IT PLAYS, and it plays the right samples. Unity ratio, no envelope: the output IS the
  // source, which is the only baseline against which everything else means anything.
  {
    Fixture f = ramp(256);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    // HARD LEFT so the pan law is unity here and this test is about PLAYBACK, not panning. A
    // centred mono source is correctly -3 dB a side (asserted separately below); reading it here
    // would make every expectation carry a 0.707 that has nothing to do with what is under test.
    spec.pan = -1.0f;
    std::vector<float> l, r;
    renderChunked(spec, 256, 64, 0, l, r);
    checkNear(l[0], f.l[0], 1e-5f, "frame 0 is the source's frame 0");
    checkNear(l[100], f.l[100], 1e-4f, "frame 100 is the source's frame 100");
    checkNear(l[200], f.l[200], 1e-4f, "frame 200 is the source's frame 200");
  }

  // ---- THE HEADLINE: BLOCK-SIZE INVARIANCE, BIT-IDENTICAL. Renders at 64, 256 and 1024 must
  // agree exactly — not approximately. A voice that starts on a block boundary fails this
  // immediately, which is why it is the check that keeps intra-block starts honest.
  {
    Fixture f = ramp(4096);
    daw::EnvShape env = daw::makeAdsr(2000, 3000, 600, 10000);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(2);
    spec.ratio = 1.4142135623730951;  // irrational, so the fraction never lands on a nice value
    spec.ampEnv = &env;
    spec.envUnitsPerFrame = 1000000.0 / 48000.0;  // microseconds per frame
    spec.pan = 0.3f;

    const uint32_t total = 2048;
    const uint32_t offset = 77;  // deliberately NOT a multiple of any block size below
    std::vector<float> l64, r64, l256, r256, l1024, r1024;
    renderChunked(spec, total, 64, offset, l64, r64);
    renderChunked(spec, total, 256, offset, l256, r256);
    renderChunked(spec, total, 1024, offset, l1024, r1024);

    check(std::memcmp(l64.data(), l256.data(), total * sizeof(float)) == 0,
          "64-frame and 256-frame renders are BIT-IDENTICAL on the left");
    check(std::memcmp(r64.data(), r256.data(), total * sizeof(float)) == 0,
          "64-frame and 256-frame renders are BIT-IDENTICAL on the right");
    check(std::memcmp(l64.data(), l1024.data(), total * sizeof(float)) == 0,
          "64-frame and 1024-frame renders are BIT-IDENTICAL on the left");
    check(std::memcmp(r64.data(), r1024.data(), total * sizeof(float)) == 0,
          "64-frame and 1024-frame renders are BIT-IDENTICAL on the right");

    // ...and the fixture actually produced signal, or memcmp would be comparing silence to
    // silence and passing for the wrong reason. This is the negative control for the control.
    float peak = 0.0f;
    for (float v : l64) {
      peak = std::max(peak, std::fabs(v));
    }
    check(peak > 0.05f, "the invariance fixture produced audible signal — comparing two silences "
                        "would pass while proving nothing");
  }

  // ---- SAMPLE-ACCURATE START. Everything before the note's frame must be EXACTLY zero. This is
  // what block-aligned starts get wrong, and it is inaudible as an error: it just moves the hit.
  {
    Fixture f = ramp(512);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    std::vector<float> l, r;
    renderChunked(spec, 512, 128, 200, l, r);
    bool cleanBefore = true;
    for (uint32_t i = 0; i < 200; ++i) {
      if (l[i] != 0.0f) {
        cleanBefore = false;
      }
    }
    check(cleanBefore, "nothing is written before the note's own sample");
    // The source starts at 0.0, so look a little past the onset for evidence it arrived.
    check(std::fabs(l[260]) > 0.0f, "and the voice IS sounding after it");
  }

  // ---- VARISPEED. Ratio 2.0 reads twice as fast, so the note is half as long — and the sample
  // at output frame i is source frame 2i.
  {
    Fixture f = ramp(1000);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 2.0;
    spec.pan = -1.0f;  // unity on the left; see above
    std::vector<float> l, r;
    renderChunked(spec, 1000, 128, 0, l, r);
    checkNear(l[100], f.l[200], 2e-3f, "at ratio 2.0, output frame 100 is source frame 200");
    checkNear(l[400], f.l[800], 2e-3f, "and output 400 is source 800");
    // Past the halfway point the source has run out, so the voice must be done rather than
    // reading off the end.
    bool silentAfter = true;
    for (uint32_t i = 520; i < 1000; ++i) {
      if (l[i] != 0.0f) {
        silentAfter = false;
      }
    }
    check(silentAfter, "and it stops when the source runs out rather than reading past it");
  }

  // ---- REVERSE walks backwards from the end. The ramp makes this unmistakable: forward starts
  // near 0 and rises, reverse starts near 1 and falls.
  {
    Fixture f = ramp(1000);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.reverse = true;
    spec.pan = -1.0f;  // unity on the left; see above
    std::vector<float> l, r;
    renderChunked(spec, 1000, 128, 0, l, r);
    check(l[0] > 0.9f, "reverse begins at the END of the sample");
    check(l[500] < l[100], "and descends through it");
  }

  // ---- STEREO STAYS STEREO, and pan on a stereo source is a BALANCE. The fixture's channels are
  // negatives of each other, so a downmix would cancel to silence — the loudest possible failure.
  {
    Fixture f = ramp(512);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(2);
    spec.ratio = 1.0;
    spec.pan = 0.0f;
    std::vector<float> l, r;
    renderChunked(spec, 512, 128, 0, l, r);
    checkNear(l[256], f.l[256], 1e-3f, "a CENTRED stereo source passes the left at UNITY — a "
                                       "constant-power law here would make every centred stereo "
                                       "sample quieter than the file");
    checkNear(r[256], f.r[256], 1e-3f, "and the right at unity");
    check(l[256] > 0.0f && r[256] < 0.0f,
          "the channels stay separate — this fixture's channels are negatives, so a downmix "
          "would cancel them to silence");
  }
  {
    Fixture f = ramp(512);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(2);
    spec.ratio = 1.0;
    spec.pan = -1.0f;  // hard left
    std::vector<float> l, r;
    renderChunked(spec, 512, 128, 0, l, r);
    checkNear(r[256], 0.0f, 1e-5f, "hard left SILENCES the right side of a stereo source");
    checkNear(l[256], f.l[256], 1e-3f, "and leaves the left at unity — a balance attenuates one "
                                       "side, it does not reposition");
  }

  // ---- A MONO SOURCE IS PLACED, NOT BALANCED. Two meanings for one control, and conflating them
  // is how a centred stereo clip comes out narrower than the file.
  {
    Fixture f = ramp(512);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.pan = 0.0f;
    std::vector<float> l, r;
    renderChunked(spec, 512, 128, 0, l, r);
    checkNear(l[256], r[256], 1e-5f, "a centred mono source is equal in both outputs");
    checkNear(l[256], f.l[256] * 0.70710678f, 2e-3f,
              "at CONSTANT POWER (-3 dB a side), not unity — a mono source is one signal being "
              "placed, and placing it at unity in both makes it 3 dB louder than panned hard");
  }

  // ---- THE AMP ENVELOPE IS APPLIED, and ramped across the block rather than stepped at its edge.
  {
    Fixture f = ramp(8000);
    for (auto& v : f.l) {
      v = 1.0f;  // DC, so the output IS the envelope and nothing else
    }
    daw::EnvShape env = daw::makeAdsr(50000, 0, 1000, 1000);  // 50 ms linear attack
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.pan = -1.0f;  // all in the left, so the pan law is out of the way
    spec.ampEnv = &env;
    spec.envUnitsPerFrame = 1000000.0 / 48000.0;
    std::vector<float> l, r;
    renderChunked(spec, 4800, 64, 0, l, r);  // 100 ms at 48k
    check(l[0] < 0.05f, "the attack starts near silence");
    checkNear(l[1200], 0.5f, 0.05f, "and is halfway up at half the attack time");
    check(l[2400] > 0.95f, "and reaches full at the end of it");
    // NO STEPS. A block-rate envelope applied without ramping shows as a staircase with a tread
    // exactly one block wide; the biggest jump between adjacent samples bounds it.
    float maxJump = 0.0f;
    for (uint32_t i = 1; i < 2400; ++i) {
      maxJump = std::max(maxJump, std::fabs(l[i] - l[i - 1]));
    }
    check(maxJump < 0.01f, "the envelope RAMPS across each block rather than stepping at its "
                           "edge — a staircase here is zipper noise");
  }

  // ---- A ONE-SHOT IGNORES NOTE-OFF. That is the difference between a drum and a pad, and it is
  // the caller's decision (gate), not the voice's.
  {
    Fixture f = ramp(4000);
    for (auto& v : f.l) {
      v = 1.0f;
    }
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.pan = -1.0f;
    daw::SamplerVoice v;
    v.start(spec, 1, 1, 0);
    std::vector<float> l(4000, 0.0f), r(4000, 0.0f);
    float* planes[2] = {l.data(), r.data()};
    v.render(planes, 2, 0, 500);
    check(v.active(), "still sounding");
    // With no envelope, release() ends the voice — a gated slot's behaviour. A ONE-SHOT slot
    // simply never calls it, which is why that decision lives in the caller.
    v.release();
    check(!v.active(), "with no envelope, note-off ends the voice");
  }

  // ---- THE STUCK-VOICE / DENORMAL FLOOR. A released voice whose envelope has decayed below
  // -120 dBFS ends, rather than holding a pool slot forever and running in the denormal range at
  // ~100x cost.
  {
    Fixture f = ramp(48000);
    for (auto& v : f.l) {
      v = 1.0f;
    }
    daw::EnvShape env = daw::makeAdsr(0, 0, 1000, 1000);  // 1 ms release
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.ampEnv = &env;
    spec.envUnitsPerFrame = 1000000.0 / 48000.0;
    daw::SamplerVoice v;
    v.start(spec, 1, 1, 0);
    std::vector<float> l(48000, 0.0f), r(48000, 0.0f);
    float* planes[2] = {l.data(), r.data()};
    v.render(planes, 2, 0, 256);
    v.release();
    for (int i = 0; i < 20 && v.active(); ++i) {
      v.render(planes, 2, 0, 256);
    }
    check(!v.active(), "a released voice whose envelope has decayed ENDS");
  }

  // ---- ...AND A RELEASE-LOOPING VOICE DOES NOT DIE AT THE FIRST TROUGH. This is the defect pair
  // the tracker survey surfaced: the same envelope was a voice leak OR a truncation depending on
  // which guard fired first. The floor must not fire while a loop is going to bring the value
  // back up; the loop is terminated by releaseFade instead.
  {
    Fixture f = ramp(96000);
    for (auto& v : f.l) {
      v = 1.0f;
    }
    daw::EnvShape env;
    env.points = {{0, 1000, 0, 0}, {20000, 0, 0, 0}, {40000, 1000, 0, 0}};
    env.sustainLoopStart = 0;
    env.sustainLoopEnd = 0;   // hold at full while held
    env.releaseLoopStart = 1;
    env.releaseLoopEnd = 2;   // then cycle down-and-up, THROUGH ZERO
    env.releaseFade = 500000; // 0.5 s terminator
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.ampEnv = &env;
    spec.envUnitsPerFrame = 1000000.0 / 48000.0;
    daw::SamplerVoice v;
    v.start(spec, 1, 1, 0);
    std::vector<float> l(96000, 0.0f), r(96000, 0.0f);
    float* planes[2] = {l.data(), r.data()};
    v.render(planes, 2, 0, 256);
    v.release();
    // Run past several loop troughs. A voice killed at the first one would stop far too early.
    int blocks = 0;
    for (; blocks < 200 && v.active(); ++blocks) {
      v.render(planes, 2, 0, 256);
    }
    check(blocks > 20, "a release-LOOPING voice survives the troughs of its own loop rather than "
                       "being killed at the first one — that would truncate the loop instead of "
                       "ending it");
    check(!v.active(), "and it still TERMINATES, via releaseFade, rather than leaking the voice");
  }

  // ---- FADE-OUT RAMPS. Choke and voice stealing use it, and an instant cut mid-waveform is a
  // click that is indistinguishable from a bug in the loop points.
  {
    Fixture f = ramp(4000);
    for (auto& v : f.l) {
      v = 1.0f;
    }
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.ratio = 1.0;
    spec.pan = -1.0f;
    daw::SamplerVoice v;
    v.start(spec, 1, 1, 0);
    std::vector<float> l(4000, 0.0f), r(4000, 0.0f);
    float* planes[2] = {l.data(), r.data()};
    v.render(planes, 2, 0, 100);
    v.fadeOut(128);
    v.render(planes, 2, 100, 200);
    check(!v.active(), "the voice ends once the fade completes");
    checkNear(l[227], 0.0f, 0.02f, "and it has reached zero by the end of the ramp");
    float maxJump = 0.0f;
    for (uint32_t i = 101; i < 228; ++i) {
      maxJump = std::max(maxJump, std::fabs(l[i] - l[i - 1]));
    }
    check(maxJump < 0.05f, "the fade is a RAMP, not a cut");
  }

  // ---- DEGENERATE INPUTS ARE INERT, not crashes. Every one of these is reachable from a project
  // file with a missing or empty sample.
  {
    daw::SamplerVoice v;
    daw::SamplerVoiceSpec spec;  // no source at all
    v.start(spec, 1, 1, 0);
    check(!v.active(), "a voice with no source never becomes active");
    std::vector<float> l(64, 0.0f), r(64, 0.0f);
    float* planes[2] = {l.data(), r.data()};
    v.render(planes, 2, 0, 64);  // must not crash
    check(l[0] == 0.0f, "and renders nothing");
  }
  {
    Fixture f = ramp(100);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.startFrame = 50;
    spec.endFrame = 50;  // empty extent
    daw::SamplerVoice v;
    v.start(spec, 1, 1, 0);
    check(!v.active(), "a zero-length extent does not start a voice that renders forever");
  }
  {
    Fixture f = ramp(100);
    daw::SamplerVoiceSpec spec;
    spec.source = f.view(1);
    spec.startFrame = 90;
    spec.endFrame = 10;  // inverted
    daw::SamplerVoice v;
    v.start(spec, 1, 1, 0);
    check(!v.active(), "an inverted extent is refused rather than read backwards by accident");
  }

  // ---- HERMITE PASSES THROUGH ITS CONTROL POINTS. If it did not, unity-ratio playback would not
  // reproduce the source, and every other assertion here would be measuring the interpolator
  // instead of the thing it claims to test.
  {
    checkNear(daw::hermite4(0.0f, 1.0f, 2.0f, 3.0f, 0.0f), 1.0f, 1e-6f,
              "hermite at t=0 IS y0");
    checkNear(daw::hermite4(0.0f, 1.0f, 2.0f, 3.0f, 1.0f), 2.0f, 1e-6f,
              "hermite at t=1 IS y1");
    checkNear(daw::hermite4(0.0f, 1.0f, 2.0f, 3.0f, 0.5f), 1.5f, 1e-6f,
              "and a linear ramp interpolates linearly — no overshoot on straight data");
  }

  if (g_fail == 0) {
    std::printf("sampler_voice_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
