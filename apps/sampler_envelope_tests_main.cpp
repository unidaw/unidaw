// THE ENVELOPE, AND THE ONE CLAIM THAT HAS TO BE TRUE: ADSR IS A VIEW OF THE SAME POINTS.
//
// docs/SAMPLER_DESIGN.md R4 rules that ADSR is not a second envelope type but four points with a
// one-point sustain loop. If that is false anywhere — if switching editors converts, approximates
// or invents — then the sampler has two envelope models pretending to be one, and every later
// feature has to ask which one it is talking to. So it is asserted here first.
//
// The rest of these pin behaviours that were REAL BUGS in the trackers being copied, plus the two
// shapes this codebase keeps rediscovering: a sentinel that collides with a legal value, and a
// degenerate case that spins instead of holding.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "apps/sampler_envelope.h"
#include "apps/sampler_state.h"

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
    std::printf("FAIL %s: got %.4f want %.4f (tol %.4f)\n", what, got, want, tol);
    ++g_fail;
  }
}

// One microsecond of envelope time per frame, so a test can say "advance 100 units" and mean it.
constexpr double kUnit = 1.0;

}  // namespace

int main() {
  // ---- THE SHAPE LOOKUP, independent of any clock.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {100, 1000, 0, 0}, {200, 0, 0, 0}};
    checkNear(daw::envValueAt(s, 0), 0.0f, 1e-5f, "value at t=0");
    checkNear(daw::envValueAt(s, 50), 0.5f, 1e-5f, "linear midpoint of the rise");
    checkNear(daw::envValueAt(s, 100), 1.0f, 1e-5f, "the peak point itself");
    checkNear(daw::envValueAt(s, 150), 0.5f, 1e-5f, "linear midpoint of the fall");
    // Before the first point and after the last, an envelope HOLDS. It does not extrapolate:
    // extrapolating a -1000..1000 curve past its ends produces values outside the domain, and
    // for a volume envelope that is a click or a phase inversion.
    checkNear(daw::envValueAt(s, -50), 0.0f, 1e-5f, "before the first point holds");
    checkNear(daw::envValueAt(s, 9999), 0.0f, 1e-5f, "after the last point holds");
  }

  // ---- STEP segments. Without these a stepped shape needs two points at the same time, which
  // the strictly-increasing invariant forbids — so the flag is what makes them expressible at all.
  {
    daw::EnvShape s;
    s.points = {{0, 1000, 0, daw::kEnvPointStep}, {100, 0, 0, 0}};
    checkNear(daw::envValueAt(s, 1), 1.0f, 1e-5f, "step holds just after its point");
    checkNear(daw::envValueAt(s, 99), 1.0f, 1e-5f, "step still holds just before the next");
    checkNear(daw::envValueAt(s, 100), 0.0f, 1e-5f, "step jumps AT the next point");
  }

  // ---- TENSION. Three properties, because a curve that fails any of them crosses its own
  // control points and the drawn shape stops matching the heard one.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {100, 1000, 0, 0}};
    daw::EnvShape easeIn = s, easeOut = s;
    easeIn.points[0].tension = 100;
    easeOut.points[0].tension = -100;
    // ENDPOINTS ARE EXACT whatever the tension. A curve that does not reach its own points is a
    // curve you cannot draw against.
    checkNear(daw::envValueAt(easeIn, 0), 0.0f, 1e-5f, "ease-in hits the start exactly");
    checkNear(daw::envValueAt(easeIn, 100), 1.0f, 1e-5f, "ease-in hits the end exactly");
    checkNear(daw::envValueAt(easeOut, 0), 0.0f, 1e-5f, "ease-out hits the start exactly");
    checkNear(daw::envValueAt(easeOut, 100), 1.0f, 1e-5f, "ease-out hits the end exactly");
    // DIRECTION: positive tension is slow-then-fast, so it sits BELOW linear in the middle.
    check(daw::envValueAt(easeIn, 50) < 0.5f, "positive tension lags linear");
    check(daw::envValueAt(easeOut, 50) > 0.5f, "negative tension leads linear");
    // SYMMETRY about 0: the two are mirror images, so a knob at +40 and -40 feels the same
    // amount of curve in each direction.
    checkNear(daw::envValueAt(easeIn, 50) + daw::envValueAt(easeOut, 50), 1.0f, 1e-4f,
              "tension is symmetric about linear");
    // MONOTONIC: never doubles back, or a rising envelope dips.
    float prev = -1.0f;
    bool mono = true;
    for (int t = 0; t <= 100; ++t) {
      const float v = daw::envValueAt(easeIn, t);
      if (v < prev - 1e-6f) {
        mono = false;
      }
      prev = v;
    }
    check(mono, "a tensioned segment is monotonic");
  }

  // ---- THE HEADLINE CLAIM: ADSR IS FOUR POINTS AND A ONE-POINT SUSTAIN LOOP.
  {
    const daw::EnvShape adsr = daw::makeAdsr(100, 100, 600, 200);
    check(adsr.points.size() == 4, "ADSR is exactly four points");
    check(adsr.sustainLoopStart == 2 && adsr.sustainLoopEnd == 2,
          "ADSR's sustain is a ZERO-LENGTH loop at the sustain point — FT2's sustain point and "
          "IT's sustain loop are the same mechanism at two lengths, which is why there is no "
          "separate sustainPoint field to disagree with these indices");
    check(adsr.releaseLoopStart == daw::kEnvLoopNone, "ADSR has no release loop");

    daw::EnvRunner r;
    r.start(&adsr, kUnit);
    checkNear(r.value(), 0.0f, 1e-5f, "ADSR starts silent");
    checkNear(r.advance(100), 1.0f, 1e-3f, "attack reaches full");
    checkNear(r.advance(100), 0.6f, 1e-3f, "decay reaches sustain");

    // THE ZERO-LENGTH LOOP HOLDS RATHER THAN WRAPPING. Wrapping by zero would spin forever on the
    // audio thread; holding is what the degenerate case MEANS. This is the assertion that would
    // have caught it as a hang instead of a silence.
    for (int i = 0; i < 50; ++i) {
      checkNear(r.advance(64), 0.6f, 1e-3f, "sustain holds while the key is down");
    }
    check(!r.finished(), "a held ADSR never finishes");

    // Release runs on from where it is — not from the release POINT, which would jump the value
    // and click.
    r.release();
    checkNear(r.advance(100), 0.3f, 2e-2f, "release is halfway down after half its time");
    checkNear(r.advance(100), 0.0f, 1e-3f, "release reaches zero");
    check(r.finished(), "an ADSR finishes once released and run out");
  }

  // ---- A FREELY DRAWN ENVELOPE REACHES THE SAME RUNNER. Same struct, same clock, no conversion
  // — which is the whole point of R4. A 6-point shape with mixed tension and a step must run.
  {
    daw::EnvShape drawn;
    drawn.points = {{0, 0, 40, 0},   {30, 900, -20, 0}, {60, 300, 0, daw::kEnvPointStep},
                    {90, 300, 0, 0}, {140, 1000, 60, 0}, {200, 0, 0, 0}};
    daw::EnvRunner r;
    r.start(&drawn, kUnit);
    r.release();  // no loops at all: it just plays through
    float last = 0.0f;
    for (int i = 0; i < 40; ++i) {
      last = r.advance(5);
    }
    checkNear(last, 0.0f, 1e-3f, "a drawn envelope with no loops plays through to its last point");
    check(r.finished(), "and finishes");
  }

  // ---- THE FT2 GESTURE: A LOOPED ENVELOPE SECTION. "you can loop an envelope section, like in
  // FT2" — the owner's words, and the reason the sustain loop is a RANGE and not just a point.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {100, 1000, 0, 0}, {200, 0, 0, 0}, {300, 0, 0, 0}};
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 2;  // cycle the whole 0 -> 1000 -> 0 triangle while held
    s.loopMode = daw::kEnvLoopForward;

    daw::EnvRunner r;
    r.start(&s, kUnit);
    // Count peaks over 8 loop periods. A loop that does not loop reads as ONE peak, which is the
    // negative control this asserts against: 8 must be 8.
    int peaks = 0;
    float prev = 0.0f, prevPrev = 0.0f;
    for (int i = 0; i < 8 * 200 / 5; ++i) {
      const float v = r.advance(5);
      if (prev > prevPrev && prev >= v && prev > 0.9f) {
        ++peaks;
      }
      prevPrev = prev;
      prev = v;
    }
    check(peaks >= 7 && peaks <= 8, "a sustain loop cycles once per period (8 periods -> ~8 peaks)");
    check(!r.finished(), "a looping held envelope never finishes");

    // And note-off LEAVES the loop and plays out. If release did not break the loop, a note would
    // sustain forever after the key came up — which is the bug that makes a tracker unusable.
    r.release();
    float v = 0.0f;
    for (int i = 0; i < 200; ++i) {
      v = r.advance(5);
    }
    checkNear(v, 0.0f, 1e-3f, "note-off leaves the sustain loop and plays out");
    check(r.finished(), "and then finishes");
  }

  // ---- PING-PONG reverses instead of jumping. The audible difference is a click at the seam, so
  // the property is that the value is CONTINUOUS across the turnaround.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {100, 1000, 0, 0}};
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 1;
    s.loopMode = daw::kEnvLoopPingPong;
    daw::EnvRunner r;
    r.start(&s, kUnit);
    float prev = r.value();
    bool sawDescent = false, continuous = true;
    for (int i = 0; i < 200; ++i) {
      const float v = r.advance(3);
      if (v < prev - 1e-6f) {
        sawDescent = true;
      }
      // 3 units per step on a 100-unit ramp is 0.03 of range; anything much larger is a jump.
      if (std::fabs(v - prev) > 0.08f) {
        continuous = false;
      }
      prev = v;
    }
    check(sawDescent, "ping-pong comes back down");
    check(continuous, "ping-pong turns around continuously — a jump at the seam is a click");
  }

  // ---- A LOOP SHORTER THAN THE BLOCK. Not exotic: a 3 ms loop at 1024 frames. The naive `if
  // (pos > end) pos -= len` handles one wrap and silently sticks past the end for the rest.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {10, 1000, 0, 0}, {1000, 0, 0, 0}};
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 1;  // a 10-unit loop
    daw::EnvRunner r;
    r.start(&s, kUnit);
    for (int i = 0; i < 20; ++i) {
      const float v = r.advance(500);  // fifty loop lengths in one span
      check(v >= -0.001f && v <= 1.001f,
            "a loop shorter than the block stays inside the loop range");
    }
  }
  {
    daw::EnvShape s;  // the same, ping-pong, which has the harder wrap arithmetic
    s.points = {{0, 0, 0, 0}, {10, 1000, 0, 0}, {1000, 0, 0, 0}};
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 1;
    s.loopMode = daw::kEnvLoopPingPong;
    daw::EnvRunner r;
    r.start(&s, kUnit);
    for (int i = 0; i < 20; ++i) {
      const float v = r.advance(500);
      check(v >= -0.001f && v <= 1.001f, "ping-pong shorter than the block stays in range");
    }
  }

  // ---- BACKWARD. The third loop mode the owner asked for, and the one that is easy to leave
  // aliased to forward because both "work" — so the fixture is DELIBERATELY ASYMMETRIC. A
  // symmetric triangle reads identically played either way and would pass with backward wired
  // straight to forward, which is the shape of test this repo keeps finding and deleting.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {50, 1000, 0, 0}, {200, 0, 0, 0}};  // fast up, slow down
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 2;
    const double len = 200.0;

    daw::EnvShape fwd = s;
    fwd.loopMode = daw::kEnvLoopForward;
    daw::EnvShape bwd = s;
    bwd.loopMode = daw::kEnvLoopBackward;

    daw::EnvRunner rf, rb;
    rf.start(&fwd, kUnit);
    rb.start(&bwd, kUnit);
    bool differs = false;
    for (int k = 1; k <= 15; ++k) {
      const float f = rf.advance(10);
      const float b = rb.advance(10);
      // The semantic, stated exactly: backward at step k sits where forward sits at (len - k·dt).
      checkNear(f, daw::envValueAt(s, k * 10.0), 1e-3f, "forward tracks the shape");
      checkNear(b, daw::envValueAt(s, len - k * 10.0), 1e-3f,
                "backward tracks the shape in reverse");
      if (std::fabs(f - b) > 1e-3f) {
        differs = true;
      }
    }
    check(differs, "backward is not silently aliased to forward");
  }

  // ---- RELEASE LOOP, AND THE TERMINATOR THAT KEEPS IT FROM LEAKING THE VOICE.
  //
  // IT has a release loop, FT2 does not, and it is the reason the two loops are separate fields
  // rather than one pair with a mode. It is ALSO where the first version of this file was wrong
  // in two directions at once, which is why the assertions below are specific:
  //
  //   the leak       a release loop cycles forever, so `finished()` never became true and the
  //                  voice was never freed.
  //   the truncation the voice's own silence-floor guard would then kill the voice at the FIRST
  //                  TROUGH of that loop — the opposite failure, from the same envelope,
  //                  depending only on which guard happened to fire first.
  //
  // The fix is the field that reads as an FT2/IT era quirk and is not: a per-envelope FADEOUT
  // after note-off. repairEnvShape() guarantees one exists whenever a release loop does, so the
  // leak is structurally impossible rather than left to the caller.
  {
    daw::EnvShape s;
    s.points = {{0, 1000, 0, 0}, {100, 0, 0, 0}, {200, 1000, 0, 0}};
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 0;  // hold at full while held
    s.releaseLoopStart = 1;
    s.releaseLoopEnd = 2;  // then cycle 0 <-> 1000
    s.releaseFade = 2000;  // ...for this long, and then stop
    daw::EnvRunner r;
    r.start(&s, kUnit);
    checkNear(r.advance(1000), 1.0f, 1e-3f, "held at the sustain point");
    check(!r.looping(), "a zero-length sustain loop is a HOLD, not a cycle");
    r.release();
    check(r.looping(), "after note-off the release loop is what is keeping it alive");

    bool moved = false;
    float prev = r.value();
    int steps = 0;
    for (; steps < 2000 && !r.finished(); ++steps) {
      const float v = r.advance(7);
      if (std::fabs(v - prev) > 1e-4f) {
        moved = true;
      }
      prev = v;
    }
    check(moved, "the release loop keeps running after note-off");
    // THE LEAK ASSERTION. Without a terminator this loop never ends and `steps` hits its cap.
    check(r.finished(), "a release-looping envelope TERMINATES via releaseFade — without it the "
                        "voice is never freed, which is a voice leak, not a long tail");
    check(steps < 2000, "and it terminates in bounded time");
    checkNear(r.value(), 0.0f, 1e-4f, "and it ends AT ZERO rather than wherever the loop was, "
                                      "because a fade that stops mid-cycle is a click");
  }

  // ---- AND THE REPAIR THAT MAKES THE LEAK UNREACHABLE. A release loop with no fade is not a
  // valid shape, so it is repaired rather than trusted — the caller cannot forget.
  {
    daw::EnvShape s;
    s.points = {{0, 1000, 0, 0}, {100, 0, 0, 0}, {200, 1000, 0, 0}};
    s.releaseLoopStart = 1;
    s.releaseLoopEnd = 2;
    s.releaseFade = 0;  // would leak
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(r.addedReleaseFade, "a release loop without a terminator is repaired, and REPORTED — "
                              "an added fade is audible and the user should be told");
    check(s.releaseFade > 0, "and the shape now terminates");
  }
  {
    // The negative control: no release loop means no fade is invented. A terminator on an
    // envelope that already ends would cut its release short.
    daw::EnvShape s = daw::makeAdsr(10, 20, 500, 30);
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(!r.addedReleaseFade, "an envelope that already terminates gets no fade added");
    check(s.releaseFade == 0, "and its releaseFade stays zero");
  }

  // ---- THE DEFAULT MOD SET SOUNDS WITHOUT BEING REPAIRED FIRST.
  //
  // makeAdsr(0, 0, 1000, 5000) — what defaultModSet mints — puts THREE of its four points at
  // t=0: the zero, the attack peak and the sustain. Only repairEnvShape nudges them apart, and
  // it is called on the LOAD path and on the SetEnvelope command path. It is NOT called when the
  // default is minted, so a sampler built by commands runs an envelope whose first three points
  // share a time, the runner holds the first of them (v=0), and the kit is mute.
  //
  // WHY THIS SURVIVED, and it is the more useful half: tools/sampler_default_sound_check.sh
  // asserts exactly this property at the audio level and PASSES — because it saves the project
  // and renders the SAVED file, so deserialization repairs the envelope before anything is
  // heard. The check could only ever see the repaired shape. The web-UI agent, playing live
  // without a save, heard silence and was right; "it works on mine" was an artifact of my
  // fixture round-tripping through the one function that fixes it.
  //
  // Asserted here at the unit level because that is the layer that can tell the two apart: no
  // save, no load, no engine — just the shape the mint produces, run.
  {
    const daw::SamplerModSet m = daw::defaultModSet(1);
    check(m.modulators.size() == 1, "the default mod set has its amp modulator");
    const daw::EnvShape& env = m.modulators.at(0).env;

    // The times must already be usable. A shape whose points share a time is not a shape the
    // runner can walk, and depending on a repair pass to make a CONSTRUCTOR's output valid is
    // the invariant being someone else's job.
    for (size_t i = 1; i < env.points.size(); ++i) {
      check(env.points[i].time > env.points[i - 1].time,
            "the default envelope's point times are strictly increasing as minted, without a "
            "repair pass — three points at t=0 is what makes a command-built kit silent");
    }

    daw::EnvRunner r;
    r.start(&env, kUnit);
    const float held = r.advance(64);
    check(held > 0.9f,
          "the default amp envelope is at full level while the key is held. It is instant-attack "
          "with full sustain, so anything less than full here means the runner landed on a point "
          "that is not the sustain — and every sampler made by commands renders silence");
    for (int i = 0; i < 20; ++i) {
      checkNear(r.advance(64), 1.0f, 1e-3f, "and it HOLDS there rather than decaying away");
    }
  }

  // ---- REPAIR IS LOUD. Every one of these was a silent clamp somewhere before it was a rule.
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {50, 2000, 0, 0}, {40, 0, 0, 0}};  // out of order + out of range
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(r.any(), "repair reports that it changed something");
    check(r.reorderedPoints == 1, "the backwards point is counted");
    check(s.points[2].time > s.points[1].time, "and the times are strictly increasing after");
    check(s.points[1].valueMilli == 1000, "an out-of-range value is clamped to the domain");
    // PINNED TO ITS NEIGHBOUR, NOT SORTED. A point dragged past its neighbour should stay where
    // the hand left it; sorting would teleport it to the far end of the envelope instead.
    check(s.points[2].time == 51, "a dragged-past point pins to its neighbour rather than sorting");
  }
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {50, 1000, 0, 0}};
    s.sustainLoopStart = 0;
    s.sustainLoopEnd = 9;  // past the end
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(r.clearedSustainLoop, "a loop index past the end is reported");
    check(s.sustainLoopStart == daw::kEnvLoopNone && s.sustainLoopEnd == daw::kEnvLoopNone,
          "and the loop is cleared as a PAIR — half a loop is not a loop");
  }
  {
    daw::EnvShape s;
    s.points = {{0, 0, 0, 0}, {50, 1000, 0, 0}, {90, 0, 0, 0}};
    s.sustainLoopStart = 2;
    s.sustainLoopEnd = 1;  // inverted
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(r.swappedSustainLoop, "an inverted loop is reported");
    check(s.sustainLoopStart == 1 && s.sustainLoopEnd == 2, "and swapped rather than dropped");
  }
  {
    daw::EnvShape s;
    for (int i = 0; i < 100; ++i) {
      s.points.push_back({static_cast<uint32_t>(i * 10), 0, 0, 0});
    }
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(s.points.size() == daw::kMaxEnvPoints, "the point cap is enforced");
    check(r.droppedPoints == 36, "and the drop is COUNTED, not silent — this codebase has "
                                 "shipped a silent truncation before");
  }
  {
    // A clean shape must report NOTHING. Without this, `any()` could be stuck true and every load
    // would fire a repair event, which is the same as none.
    daw::EnvShape s = daw::makeAdsr(10, 20, 500, 30);
    const daw::EnvRepair r = daw::repairEnvShape(s);
    check(!r.any(), "a well-formed envelope repairs nothing (the negative control for repair)");
    check(s.sustainLoopStart == 2 && s.sustainLoopEnd == 2,
          "and repair does not clear a legal ZERO-LENGTH loop — the ADSR case runs through "
          "repair on every load, so treating start==end as malformed would silently turn every "
          "ADSR into a one-shot");
  }

  // ---- AN EMPTY ENVELOPE IS INERT, not a crash and not a hang. Reached by any device whose
  // modulator list is default-constructed.
  {
    daw::EnvShape s;
    daw::EnvRunner r;
    r.start(&s, kUnit);
    checkNear(r.advance(1000), 0.0f, 1e-6f, "an empty envelope is silent");
    check(r.finished(), "and finishes immediately rather than holding a voice open forever");
  }

  if (g_fail == 0) {
    std::printf("sampler_envelope_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
