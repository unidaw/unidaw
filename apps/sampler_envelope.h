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

// The drawable shape. Time unit is the owning modulator's `timeBase`; this struct does not know
// whether a tick is a microsecond or a nanotick, and does not need to.
struct EnvShape {
  std::vector<EnvPoint> points;
  uint8_t sustainLoopStart = kEnvLoopNone, sustainLoopEnd = kEnvLoopNone;
  uint8_t releaseLoopStart = kEnvLoopNone, releaseLoopEnd = kEnvLoopNone;
  uint8_t loopMode = kEnvLoopForward;  // applies to whichever loop is running

  bool empty() const { return points.empty(); }
  uint32_t duration() const { return points.empty() ? 0u : points.back().time; }
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
  bool any() const {
    return reorderedPoints || droppedPoints || clearedSustainLoop ||
           clearedReleaseLoop || swappedSustainLoop || swappedReleaseLoop;
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
  return r;
}

// Value at an arbitrary time, ignoring loops. The runner owns looping (it needs direction state
// for ping-pong); this is the pure shape lookup, which is why it can be tested on its own.
inline float envValueAt(const EnvShape& s, double t) {
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
  while (i + 1 < s.points.size() &&
         static_cast<double>(s.points[i + 1].time) <= t) {
    ++i;
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

// The clock. Block-rate: advance() moves time forward by a span and returns the value at the end
// of it, so the caller ramps from the previous value across the block.
class EnvRunner {
 public:
  // `unitsPerFrame` converts the host's frames to the envelope's time unit — microseconds per
  // frame, or nanoticks per frame. Keeping the conversion at the boundary is what lets timeBase
  // be one field on the modulator instead of a flag the runner has to branch on.
  void start(const EnvShape* shape, double unitsPerFrame) {
    shape_ = shape;
    unitsPerFrame_ = unitsPerFrame;
    pos_ = shape_ && !shape_->points.empty()
               ? static_cast<double>(shape_->points.front().time)
               : 0.0;
    held_ = true;
    forward_ = true;
    done_ = false;
    value_ = shape_ ? envValueAt(*shape_, pos_) : 0.0f;
  }

  // Note-off. The sustain loop stops holding and time runs on from exactly where it is — not from
  // the release point, which would jump the value and click.
  void release() { held_ = false; }

  bool active() const { return shape_ != nullptr && !done_; }
  bool finished() const { return done_; }
  float value() const { return value_; }

  float advance(uint32_t frames) {
    if (!shape_ || shape_->points.empty()) {
      value_ = 0.0f;
      done_ = true;
      return value_;
    }
    double dt = static_cast<double>(frames) * unitsPerFrame_;
    // A zero-length loop HOLDS. Wrapping by zero would spin forever, and "stay here while the key
    // is down" is precisely what FT2's single sustain point means — so the degenerate case is the
    // feature, not an error to reject.
    uint8_t la = kEnvLoopNone, lb = kEnvLoopNone;
    if (held_ && shape_->sustainLoopStart != kEnvLoopNone) {
      la = shape_->sustainLoopStart;
      lb = shape_->sustainLoopEnd;
    } else if (!held_ && shape_->releaseLoopStart != kEnvLoopNone) {
      la = shape_->releaseLoopStart;
      lb = shape_->releaseLoopEnd;
    }
    if (la != kEnvLoopNone && la < shape_->points.size() && lb < shape_->points.size()) {
      const double ta = static_cast<double>(shape_->points[la].time);
      const double tb = static_cast<double>(shape_->points[lb].time);
      const double len = tb - ta;
      if (len <= 0.0) {
        // Hold. Only advance up TO the hold point; once there, time stops.
        pos_ = std::min(pos_ + dt, ta);
        value_ = envValueAt(*shape_, pos_);
        return value_;
      }
      // Guard the loop arithmetic against a span longer than the loop itself — a tiny loop at a
      // large block size is not exotic, it is a 3 ms loop at 1024 frames.
      if (shape_->loopMode == kEnvLoopPingPong) {
        while (dt > 0.0) {
          const double room = forward_ ? (tb - pos_) : (pos_ - ta);
          const double step = std::min(dt, std::max(room, 0.0));
          pos_ += forward_ ? step : -step;
          dt -= step;
          if (dt > 0.0 || step >= room) {
            forward_ = !forward_;
            pos_ = std::clamp(pos_, ta, tb);
            if (step <= 0.0 && dt > 0.0) {
              // Degenerate: no room in either direction. Cannot happen with len > 0, but a
              // spin here would be an audio-thread hang, so it ends rather than trusting that.
              break;
            }
          }
        }
      } else if (shape_->loopMode == kEnvLoopBackward) {
        pos_ -= dt;
        while (pos_ < ta) {
          pos_ += len;
        }
      } else {
        pos_ += dt;
        while (pos_ > tb) {
          pos_ -= len;
        }
      }
      value_ = envValueAt(*shape_, pos_);
      return value_;
    }
    pos_ += dt;
    const double end = static_cast<double>(shape_->points.back().time);
    if (pos_ >= end) {
      pos_ = end;
      // Finished means "will never change again", which is only true once the key is up. A held
      // note that has run off the end of its envelope is still sounding at the last value.
      if (!held_) {
        done_ = true;
      }
    }
    value_ = envValueAt(*shape_, pos_);
    return value_;
  }

 private:
  const EnvShape* shape_ = nullptr;
  double unitsPerFrame_ = 0.0;
  double pos_ = 0.0;
  float value_ = 0.0f;
  bool held_ = false;
  bool forward_ = true;
  bool done_ = false;
};

}  // namespace daw
