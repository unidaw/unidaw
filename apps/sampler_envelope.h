#pragma once

// MULTIPOINT, FREELY DRAWN, LOOPABLE ENVELOPES — and ADSR is a VIEW of them, not a second type.
//
// The owner's spec: "the envelopes should be multipoint (draw env curve freely) and loopable, so
// you can loop an envelope section, like in FT2. or you can choose ADSR if you want to make it
// simpler." The ruling that makes that one feature instead of two is in docs/SAMPLER_DESIGN.md R4:
//
//     ADSR is not a different envelope type. It is four points with a one-point sustain loop.
//
// The ADSR editor writes exactly the points the pencil writes, so switching editors is a view
// change and not a conversion — nothing is lost turning a drawn curve into "the nearest ADSR" and
// nothing is invented going back. There is one structure here for that reason.
//
// TWO LOOPS, AND THAT IS THE WHOLE MODEL:
//
//   sustain loop   runs while the key is held        FT2's sustain POINT is start == end
//   release loop   runs after note-off               IT's regular loop; FT2 has no equivalent
//   neither        plays through and holds the last point
//
// FT2's sustain point and IT's sustain loop are the same mechanism at two lengths, which is why
// there is no separate `sustainPoint` field that could disagree with the loop indices. A zero-
// length loop HOLDS rather than wrapping — wrapping by zero would spin forever, and "hold here
// until note-off" is exactly what the degenerate case should mean.
//
// EVALUATION IS BLOCK-RATE. advance() returns the value at the END of the span and the caller
// ramps to it across the block. Per-sample evaluation of a 64-point envelope buys nothing audible
// and costs a segment search per sample; a linear ramp between block values is what every sampler
// worth copying does, and it is what keeps this off the profile.
//
// WHAT IS NOT HERE: depth, target, and the modulator's identity. Those belong to SamplerModulator
// (apps/sampler_state.h) — this file is the shape and the clock, so it can be tested with no
// sampler, no device, and no engine.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace daw {

// A point on a freely-drawn envelope. Time is STRICTLY INCREASING: the editor enforces it, the
// loader repairs it and says so, and the runner below assumes it. All three have to agree or the
// segment search walks backwards.
struct EnvPoint {
  uint32_t time = 0;       // in the envelope's timeBase unit, measured from t = 0
  int16_t valueMilli = 0;  // -1000..1000; the modulator's target gives it meaning
  // Toward the NEXT point. 0 = linear, positive = ease-in (slow then fast), negative = ease-out.
  // One signed byte covers every curve anyone draws; a spline would need a second control point
  // per segment and nobody has ever asked a sampler for one.
  int8_t tension = 0;
  uint8_t flags = 0;
};

// On the segment's START point: hold this value until the next point's time, then jump. This is
// what makes stepped/sample-and-hold shapes drawable without a second envelope kind.
inline constexpr uint8_t kEnvPointStep = 1u << 0;

// 0xFF, not 0, because 0 is a perfectly good point index. A sentinel that collides with a legal
// value is the bug this codebase keeps finding in other people's contracts.
inline constexpr uint8_t kEnvLoopNone = 0xFF;

inline constexpr uint8_t kEnvLoopForward = 1;
inline constexpr uint8_t kEnvLoopPingPong = 2;
inline constexpr uint8_t kEnvLoopBackward = 3;

// 64 is generous against every tracker worth matching (FT2 12, IT 25, Renoise unbounded in
// principle and ~16 in practice) and it bounds the SHM publish, which an unbounded vector cannot.
inline constexpr size_t kMaxEnvPoints = 64;

// Default terminator for a release-looping envelope. Long enough to be a musical tail rather
// than a cut, short enough that a stuck voice is impossible.
inline constexpr uint32_t kDefaultReleaseFade = 500000;  // 0.5 s in the shape's time unit

// The drawable shape. Time unit is the owning modulator's `timeBase`; this struct does not know
// whether a tick is a microsecond or a nanotick, and does not need to.
struct EnvShape {
  std::vector<EnvPoint> points;
  uint8_t sustainLoopStart = kEnvLoopNone, sustainLoopEnd = kEnvLoopNone;
  uint8_t releaseLoopStart = kEnvLoopNone, releaseLoopEnd = kEnvLoopNone;
  uint8_t loopMode = kEnvLoopForward;  // applies to whichever loop is running

  // THE TERMINATOR, and the reason FT2's and IT's weirdest field is not an era quirk.
  //
  // A RELEASE loop runs forever by definition: after note-off the envelope cycles and never
  // reaches a last point, so `finished()` would never become true and the voice would never be
  // freed. IT and FT2 both solve this with a per-instrument FADEOUT — a linear countdown after
  // note-off that multiplies everything — and that is exactly the missing piece here.
  //
  // Applied ONLY after release, and only when a release loop is what is keeping the envelope
  // alive. repairEnvShape() guarantees it is non-zero whenever a release loop is set, so a voice
  // leak is structurally impossible rather than dependent on the caller remembering.
  //
  // The numeric scaling is deliberately NOT ported from FT2 or IT: the two disagree with each
  // other and the values do not port between the formats anyway, so copying either would be
  // importing an incompatibility for no benefit.
  uint32_t releaseFade = 0;  // in this shape's time unit; 0 = none

  bool empty() const { return points.empty(); }
  uint32_t duration() const { return points.empty() ? 0u : points.back().time; }
  bool hasReleaseLoop() const { return releaseLoopStart != kEnvLoopNone; }
};

// ADSR AS POINTS. Not a conversion and not a second representation — this is what the simple
// editor writes, and the pencil can then move any of the four freely without a mode change.
//
//   A                                     0: t=0          v=0
//   |‾\                                   1: t=A          v=1000
//   |  \____________                      2: t=A+D        v=sustain
//   |  D      S      \  R                 3: t=A+D+R      v=0
//   |________________ \___              sustainLoop = {2,2}  -> hold at point 2 while held
//    ^        ^          ^              releaseLoop = none
//    0        2..2 held  note-off
inline EnvShape makeAdsr(uint32_t attack,
                         uint32_t decay,
                         int16_t sustainMilli,
                         uint32_t release) {
  EnvShape s;
  s.points.push_back({0, 0, 0, 0});
  s.points.push_back({attack, 1000, 0, 0});
  s.points.push_back({attack + decay, sustainMilli, 0, 0});
  s.points.push_back({attack + decay + release, 0, 0, 0});
  // STRICTLY INCREASING, HERE, because a zero attack and a zero decay are not an error — they
  // are the most common ADSR anyone asks for, and they put points 0, 1 and 2 all at t=0.
  //
  // This used to be left to repairEnvShape, and that is the whole defect: repair runs on the
  // LOAD path and on the SetEnvelope command path, and NOT where defaultModSet mints its amp
  // envelope. So a sampler built by commands — add a device, load a sample, play a note — ran an
  // envelope whose first three points shared a time, the runner held the first of them at v=0,
  // and the kit was silent with every structural fact about it correct: the mod set existed, the
  // read-back reported the amp bit set, a voice started.
  //
  // A constructor that emits a shape only a later pass makes valid is the invariant being
  // somebody else's job. The nudge is repairEnvShape's own rule (pin to the neighbour + 1, do
  // not sort) so the two can never disagree about what a valid shape is.
  for (size_t i = 1; i < s.points.size(); ++i) {
    if (s.points[i].time <= s.points[i - 1].time) {
      s.points[i].time = s.points[i - 1].time + 1;
    }
  }
  s.sustainLoopStart = 2;
  s.sustainLoopEnd = 2;
  return s;
}

// What repairEnvShape had to change. Reported, never silent: a clamped envelope is a sound you
// cannot explain, and this codebase has the MarkerList::repaired_ precedent for saying so.
struct EnvRepair {
  uint32_t reorderedPoints = 0;  // times that were not strictly increasing
  uint32_t droppedPoints = 0;    // beyond kMaxEnvPoints
  bool clearedSustainLoop = false;
  bool clearedReleaseLoop = false;
  bool swappedSustainLoop = false;  // start > end
  bool swappedReleaseLoop = false;
  bool addedReleaseFade = false;  // a release loop with no terminator would leak the voice
  bool any() const {
    return reorderedPoints || droppedPoints || clearedSustainLoop ||
           clearedReleaseLoop || swappedSustainLoop || swappedReleaseLoop ||
           addedReleaseFade;
  }
};

// Makes a loaded shape satisfy what the runner assumes. Called at load and after any edit that
// could break the invariant; the caller fires sampler.envelope_repaired if `any()`.
inline EnvRepair repairEnvShape(EnvShape& s) {
  EnvRepair r;
  if (s.points.size() > kMaxEnvPoints) {
    r.droppedPoints = static_cast<uint32_t>(s.points.size() - kMaxEnvPoints);
    s.points.resize(kMaxEnvPoints);
  }
  // Strictly increasing, by nudging rather than sorting. A point whose time went backwards was
  // almost certainly dragged past its neighbour, and pinning it to the neighbour keeps it where
  // the hand left it; sorting would teleport it to the far end of the envelope instead.
  for (size_t i = 1; i < s.points.size(); ++i) {
    if (s.points[i].time <= s.points[i - 1].time) {
      s.points[i].time = s.points[i - 1].time + 1;
      ++r.reorderedPoints;
    }
  }
  for (auto& p : s.points) {
    p.valueMilli = std::clamp<int16_t>(p.valueMilli, -1000, 1000);
    p.tension = std::clamp<int8_t>(p.tension, -100, 100);
  }
  const uint8_t n = static_cast<uint8_t>(std::min<size_t>(s.points.size(), 255));
  auto fixLoop = [&](uint8_t& a, uint8_t& b, bool& cleared, bool& swapped) {
    if (a == kEnvLoopNone && b == kEnvLoopNone) {
      return;
    }
    if (a == kEnvLoopNone || b == kEnvLoopNone || a >= n || b >= n) {
      a = b = kEnvLoopNone;
      cleared = true;
      return;
    }
    if (a > b) {
      std::swap(a, b);
      swapped = true;
    }
  };
  fixLoop(s.sustainLoopStart, s.sustainLoopEnd, r.clearedSustainLoop, r.swappedSustainLoop);
  fixLoop(s.releaseLoopStart, s.releaseLoopEnd, r.clearedReleaseLoop, r.swappedReleaseLoop);
  if (s.loopMode < kEnvLoopForward || s.loopMode > kEnvLoopBackward) {
    s.loopMode = kEnvLoopForward;
  }
  // A RELEASE LOOP WITHOUT A TERMINATOR IS A VOICE LEAK, so the invariant is enforced here
  // rather than trusted to whoever built the shape. After note-off a release loop cycles
  // forever and the envelope never reaches a last point, so nothing else would ever free the
  // voice. Repaired loudly, like every other repair, because a fade the user did not ask for
  // is audible and they should be told it was added.
  if (s.hasReleaseLoop() && s.releaseFade == 0) {
    s.releaseFade = kDefaultReleaseFade;
    r.addedReleaseFade = true;
  }
  return r;
}

// Value at an arbitrary time, ignoring loops. The runner owns looping; this is the pure shape
// lookup, which is why it can be tested on its own.
//
// `hint` is an in/out cursor: the caller passes the segment index it used last time. Envelopes
// are evaluated PER SAMPLE (see EnvRunner) and time almost always advances within the same
// segment, so the common case is one comparison. Without it a 64-point envelope would cost a
// linear scan per sample per voice — 196M comparisons/second at 64 voices, which is how a
// "cheap" envelope ends up on the profile. A wrong hint costs correctness nothing: it falls
// back to a binary search.
inline float envValueAt(const EnvShape& s, double t, size_t* hint = nullptr) {
  if (s.points.empty()) {
    return 0.0f;
  }
  if (t <= static_cast<double>(s.points.front().time)) {
    return static_cast<float>(s.points.front().valueMilli) / 1000.0f;
  }
  if (t >= static_cast<double>(s.points.back().time)) {
    return static_cast<float>(s.points.back().valueMilli) / 1000.0f;
  }
  size_t i = 0;
  const size_t n = s.points.size();
  bool found = false;
  if (hint && *hint + 1 < n) {
    const size_t h = *hint;
    if (static_cast<double>(s.points[h].time) <= t &&
        t < static_cast<double>(s.points[h + 1].time)) {
      i = h;
      found = true;
    }
  }
  if (!found) {
    size_t lo = 0, hi = n - 1;  // last index whose time is <= t
    while (lo < hi) {
      const size_t mid = (lo + hi + 1) / 2;
      if (static_cast<double>(s.points[mid].time) <= t) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    i = lo;
    if (hint) {
      *hint = i;
    }
  }
  const EnvPoint& a = s.points[i];
  const EnvPoint& b = s.points[std::min(i + 1, s.points.size() - 1)];
  if (a.flags & kEnvPointStep) {
    return static_cast<float>(a.valueMilli) / 1000.0f;
  }
  const double span = static_cast<double>(b.time) - static_cast<double>(a.time);
  double u = span > 0.0 ? (t - static_cast<double>(a.time)) / span : 0.0;
  u = std::clamp(u, 0.0, 1.0);
  if (a.tension != 0) {
    // Monotonic, symmetric about tension 0, and 1:1 at the endpoints — the three properties that
    // stop a drawn curve from overshooting or crossing its own control points.
    const double k = 1.0 + std::abs(static_cast<double>(a.tension)) / 100.0 * 3.0;
    u = a.tension > 0 ? std::pow(u, k) : 1.0 - std::pow(1.0 - u, k);
  }
  const double va = static_cast<double>(a.valueMilli) / 1000.0;
  const double vb = static_cast<double>(b.valueMilli) / 1000.0;
  return static_cast<float>(va + (vb - va) * u);
}

// THE CLOCK — AND IT IS A PURE FUNCTION OF ELAPSED FRAMES, WHICH IS NOT A STYLE CHOICE.
//
// The first version of this class was a block-rate accumulator: advance(n) moved an internal
// position and returned the value at the end of the span, and the voice ramped linearly to it
// across the block. That is what most samplers do, and it CANNOT satisfy the determinism property
// this design commits to (§3.5): one note rendered at 64, 256 and 1024 frames must be
// bit-identical.
//
// The reason is exact and unfixable within that shape. A linear ramp between block boundaries
// CUTS THE CORNER wherever an envelope breakpoint falls inside a block — and where breakpoints
// fall relative to block boundaries is precisely what changes with block size. The attack/decay
// corner of an ADSR lands mid-block at 1024 and on a boundary at 64, so the two renders differ.
// The sampler_voice_tests invariance check found this immediately; no ear would have.
//
// So the envelope is evaluated PER SAMPLE, from a pure function of the frame index:
//
//     valueAt(f)  depends only on (shape, unitsPerFrame, f, releasedAt)
//
// which makes blocking irrelevant by construction rather than by care. Everything that used to be
// accumulator state — loop position, ping-pong direction, the release fade — is derived from `f`
// with modular arithmetic, so there is no drift, no dependence on call history, and a voice can be
// evaluated at any frame in any order and give the same answer.
//
// The cost objection the old comment raised is answered by the cursor hint in envValueAt(): the
// common case is one comparison per sample, because time almost always stays in the same segment.
class EnvRunner {
 public:
  static constexpr uint64_t kNotReleased = ~0ull;

  // `unitsPerFrame` converts the host's frames to the envelope's time unit — microseconds per
  // frame, or nanoticks per frame. Keeping the conversion at the boundary is what lets timeBase
  // be one field on the modulator instead of a branch the runner has to carry.
  void start(const EnvShape* shape, double unitsPerFrame) {
    shape_ = shape;
    unitsPerFrame_ = unitsPerFrame;
    elapsed_ = 0;
    releasedAt_ = kNotReleased;
    hint_ = 0;
    done_ = false;
    value_ = shape_ ? valueAt(0) : 0.0f;
  }

  // Note-off. Records WHEN, rather than mutating a position — the value keeps running on from
  // exactly where it is, and "where it is" stays a function of the frame index.
  void release() { releaseAt(elapsed_); }

  // Note-off AT AN EXACT FRAME. This overload is the one a per-sample caller must use, and the
  // difference is not cosmetic: `elapsed_` is only advanced at the END of a render call, so a
  // note-off applied part-way through a block would otherwise be recorded at the previous
  // block's boundary. The release would then start at a different frame depending on the buffer
  // size — inaudible, and exactly what tools/sampler_determinism_check.sh exists to catch. It
  // did catch it, end-to-end, after the unit tests had passed.
  void releaseAt(uint64_t frame) {
    if (releasedAt_ != kNotReleased) {
      return;
    }
    releasedAt_ = frame;
    // Nothing else to arm: the release fade is DERIVED from (frame - releasedAt_) inside
    // valueAt(), like everything else here. A countdown member would be accumulator state, and
    // accumulator state is what made this class block-size dependent in the first place.
  }

  bool active() const { return shape_ != nullptr && !done_; }
  bool finished() const { return done_; }
  float value() const { return value_; }
  bool released() const { return releasedAt_ != kNotReleased; }

  // Is a loop currently keeping this envelope alive? The voice's silence-floor guard must not
  // fire at the bottom of a loop's cycle — a looping envelope that dips through zero is going to
  // come back up, and killing the voice there truncates the loop instead of ending it.
  bool looping() const {
    if (!shape_) {
      return false;
    }
    const bool rel = released();
    const uint8_t a = rel ? shape_->releaseLoopStart : shape_->sustainLoopStart;
    const uint8_t b = rel ? shape_->releaseLoopEnd : shape_->sustainLoopEnd;
    if (a == kEnvLoopNone || a >= shape_->points.size() || b >= shape_->points.size()) {
      return false;
    }
    // A zero-length loop is a HOLD, not a cycle: it never rises again on its own, so the silence
    // floor is free to end a voice sitting in one.
    return shape_->points[b].time > shape_->points[a].time;
  }

  // THE PURE EVALUATOR. `f` is frames since note-on. Depends on nothing else.
  float valueAt(uint64_t f) const {
    if (!shape_ || shape_->points.empty()) {
      return 0.0f;
    }
    double t;
    if (releasedAt_ == kNotReleased || f <= releasedAt_) {
      t = heldTime(static_cast<double>(f) * unitsPerFrame_);
    } else {
      const double tAtRelease = heldTime(static_cast<double>(releasedAt_) * unitsPerFrame_);
      const double since = static_cast<double>(f - releasedAt_) * unitsPerFrame_;
      t = releasedTime(tAtRelease, since);
    }
    float v = envValueAt(*shape_, t, &hint_);
    if (releasedAt_ != kNotReleased && f > releasedAt_ && shape_->hasReleaseLoop() &&
        shape_->releaseFade > 0) {
      const double since = static_cast<double>(f - releasedAt_) * unitsPerFrame_;
      const double fade = static_cast<double>(shape_->releaseFade);
      v *= since >= fade ? 0.0f : static_cast<float>(1.0 - since / fade);
    }
    return v;
  }

  // Would the envelope be finished at frame `f`? Also pure — the voice uses it to decide whether
  // to keep a slot, and it must agree with valueAt() at every frame.
  bool finishedAt(uint64_t f) const {
    if (!shape_ || shape_->points.empty()) {
      return true;
    }
    if (releasedAt_ == kNotReleased || f <= releasedAt_) {
      return false;  // a held note is never finished, whatever its value
    }
    const double since = static_cast<double>(f - releasedAt_) * unitsPerFrame_;
    if (shape_->hasReleaseLoop()) {
      // THE TERMINATOR. A release loop cycles forever, so without this the voice would never be
      // freed. repairEnvShape() guarantees releaseFade is set whenever a release loop is.
      return shape_->releaseFade > 0 && since >= static_cast<double>(shape_->releaseFade);
    }
    const double tAtRelease = heldTime(static_cast<double>(releasedAt_) * unitsPerFrame_);
    return tAtRelease + since >= static_cast<double>(shape_->points.back().time);
  }

  // Moves the clock forward and returns the value at the END of the span, for callers that work
  // a block at a time. A per-sample caller uses valueAt(age + i) directly and never touches this.
  float advance(uint32_t frames) {
    if (!shape_ || shape_->points.empty()) {
      value_ = 0.0f;
      done_ = true;
      return value_;
    }
    elapsed_ += frames;
    value_ = valueAt(elapsed_);
    if (finishedAt(elapsed_)) {
      done_ = true;
      if (shape_->hasReleaseLoop()) {
        value_ = 0.0f;  // the fade reached zero; end AT zero rather than mid-cycle, or it clicks
      }
    }
    return value_;
  }

  uint64_t elapsed() const { return elapsed_; }

  // For a per-sample caller: keep the clock in step with the frames it has actually rendered,
  // so release() records the right frame and finished() agrees with valueAt().
  void advanceTo(uint64_t f) { elapsed_ = f; }

 private:
  // Folds a position into a loop range. Pure, closed-form, and identical whether it is called
  // once per block or once per sample — which is the whole point.
  static double fold(double x, double len, uint8_t mode) {
    if (mode == kEnvLoopPingPong) {
      const double period = len * 2.0;
      double y = std::fmod(x, period);
      if (y < 0.0) {
        y += period;
      }
      return y < len ? y : period - y;  // triangle: up then back down
    }
    double y = std::fmod(x, len);
    if (y < 0.0) {
      y += len;
    }
    return mode == kEnvLoopBackward ? len - y : y;
  }

  // Where the envelope is at raw time `t`, while the key is held.
  double heldTime(double t) const {
    const uint8_t a = shape_->sustainLoopStart, b = shape_->sustainLoopEnd;
    if (a == kEnvLoopNone || a >= shape_->points.size() || b >= shape_->points.size()) {
      return t;
    }
    const double ta = static_cast<double>(shape_->points[a].time);
    const double tb = static_cast<double>(shape_->points[b].time);
    const double len = tb - ta;
    if (len <= 0.0) {
      // FT2's sustain POINT: hold. Wrapping by zero would spin forever, and "stay here while the
      // key is down" is exactly what the degenerate case means — so it is the feature.
      return std::min(t, ta);
    }
    if (shape_->loopMode == kEnvLoopBackward) {
      return t <= ta ? t : tb - fold(t - ta, len, kEnvLoopForward);
    }
    return t <= tb ? t : ta + fold(t - ta, len, shape_->loopMode);
  }

  // Where it is `since` units after note-off, having been at `tAtRelease` when the key came up.
  double releasedTime(double tAtRelease, double since) const {
    const double t = tAtRelease + since;
    const uint8_t a = shape_->releaseLoopStart, b = shape_->releaseLoopEnd;
    if (a == kEnvLoopNone || a >= shape_->points.size() || b >= shape_->points.size()) {
      return t;
    }
    const double ta = static_cast<double>(shape_->points[a].time);
    const double tb = static_cast<double>(shape_->points[b].time);
    const double len = tb - ta;
    if (len <= 0.0) {
      return std::min(t, ta);
    }
    if (shape_->loopMode == kEnvLoopBackward) {
      return t <= ta ? t : tb - fold(t - ta, len, kEnvLoopForward);
    }
    return t <= tb ? t : ta + fold(t - ta, len, shape_->loopMode);
  }

  const EnvShape* shape_ = nullptr;
  double unitsPerFrame_ = 0.0;
  uint64_t elapsed_ = 0;
  uint64_t releasedAt_ = kNotReleased;
  mutable size_t hint_ = 0;  // segment cursor; a wrong value costs a binary search, never a wrong answer
  float value_ = 0.0f;
  bool done_ = false;
};

}  // namespace daw
