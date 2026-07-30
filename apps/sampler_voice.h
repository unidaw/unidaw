#pragma once

// ONE SAMPLER VOICE. Planar float in, planar float out, and it knows nothing about the engine,
// the device chain, SHM or the audio callback — which is why its tests run in milliseconds
// against no fixture. docs/SAMPLER_DESIGN.md §3.5: if a sampler bug can only be reproduced
// through a running engine, the split is in the wrong place.
//
// POSITION IS 32.32 FIXED POINT, NOT A double. Not for precision — a double is exact to 2^52
// frames, far past any sample anyone will load. It is because the octave mip-map (S3) needs the
// canonical position in ONE domain and a right-shift to be exact: the level-L read index is
// `pos >> L`, exact by construction, so a pitch envelope sweeping through an octave boundary does
// not jump. Choosing the representation now costs nothing and choosing it later is a rewrite.
//
// LOOPS (S3) ARE ALL THREE MODES AT ONCE — forward, ping-pong and backward — because shipping
// forward alone and aliasing the others to it is precisely the failure this repo keeps deleting,
// and it is invisible: a backward loop wired to forward still loops, still sounds like a loop,
// and is simply not what was asked for. The tests use an ASYMMETRIC fixture for the same reason.
//
// THE INTERPOLATOR READS ACROSS THE SEAM. At a loop boundary the four taps come from the WRAPPED
// positions, not from clamped ones. Clamping at the seam is where samplers click, and the click
// is at exactly the loop rate, so it reads as part of the sound rather than as a defect.
//
// STILL NOT HERE: the mip-map, the filter, sinc. Named so the absence is deliberate.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "apps/sampler_envelope.h"

namespace daw {

// The audio a voice reads. Borrowed, not owned: lifetime belongs to the SamplerRender snapshot
// that the audio thread holds (§3.5), so a voice never touches a refcount.
struct SamplerSourceView {
  const float* const* planes = nullptr;
  uint32_t channels = 0;
  uint64_t frames = 0;
  double sampleRate = 0.0;

  bool valid() const { return planes != nullptr && channels > 0 && frames > 0; }
};

// Everything a note-on fixes for the life of the voice.
struct SamplerVoiceSpec {
  SamplerSourceView source;
  uint64_t startFrame = 0;
  uint64_t endFrame = 0;  // exclusive; 0 means "to the end of the source"
  double ratio = 1.0;     // varispeed, INCLUDING the source/engine rate conversion
  float gain = 1.0f;      // linear
  float pan = 0.0f;       // -1 left .. +1 right
  bool reverse = false;
  // LOOP, in level-0 source frames. loopMode 0 = off, 1 forward, 2 ping-pong, 3 backward.
  // The loop is only entered once playback reaches loopEnd; before that the slot plays normally
  // from startFrame, which is what makes "attack then loop" the natural shape.
  uint64_t loopStart = 0, loopEnd = 0;
  uint32_t loopXfade = 0;   // equal-power crossfade over the seam, in frames; 0 = none
  uint8_t loopMode = 0;
  // 1 = the loop RELEASES at note-off and plays out to endFrame. 0 = it loops forever until the
  // voice ends some other way. This is the difference between a sustained pad and a drone.
  uint8_t sustainLoop = 0;
  const EnvShape* ampEnv = nullptr;  // borrowed from the snapshot; null = no envelope
  double envUnitsPerFrame = 0.0;
};

// -120 dBFS. An amp envelope below this is inaudible, and a voice that stays there is two bugs at
// once: it holds a pool slot forever, and its arithmetic runs in the denormal range at roughly
// 100x the cost of normal floats (§3.5 — nothing else in this repo handles denormals). Ending the
// voice fixes both, and it is the cheaper of the two available fixes.
inline constexpr float kVoiceSilenceFloor = 1.0e-6f;

// 4-point cubic Hermite. `Fast` quality in §3: no ringing, cheap, right for drums and slices, and
// the default. Fixes pitching DOWN; pitching UP is the mip-map's job in S3, and conflating those
// two is the single most common sampler mistake.
inline float hermite4(float ym1, float y0, float y1, float y2, float t) {
  const float c0 = y0;
  const float c1 = 0.5f * (y1 - ym1);
  const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
  return ((c3 * t + c2) * t + c1) * t + c0;
}

class SamplerVoice {
 public:
  bool active() const { return active_; }
  uint32_t noteId() const { return noteId_; }
  uint8_t column() const { return column_; }
  uint16_t slotId() const { return slotId_; }
  // Frames rendered since note-on. The tiebreak for "steal the oldest" — a musical quantity, not
  // wall-clock, so a bounce steals the same voice the audition did (§3.5 determinism).
  uint64_t age() const { return age_; }

  void start(const SamplerVoiceSpec& spec,
             uint32_t noteId,
             uint16_t slotId,
             uint8_t column) {
    spec_ = spec;
    noteId_ = noteId;
    slotId_ = slotId;
    column_ = column;
    age_ = 0;
    active_ = spec.source.valid();
    if (!active_) {
      return;
    }
    const uint64_t last = spec_.source.frames;
    endFrame_ = (spec_.endFrame == 0 || spec_.endFrame > last) ? last : spec_.endFrame;
    startFrame_ = std::min(spec_.startFrame, endFrame_);
    if (startFrame_ >= endFrame_) {
      active_ = false;
      return;
    }
    // Reverse starts at the far end and walks back. Handled as a direction rather than a second
    // read path, so every other rule (interpolation, bounds, envelope) has exactly one form.
    pos_ = (spec_.reverse ? (endFrame_ - 1) : startFrame_) << 32;
    step_ = static_cast<uint64_t>(std::max(spec_.ratio, 0.0) * 4294967296.0);
    // THE LOOP IS VALIDATED HERE, ONCE, rather than guarded at every read. An inverted or
    // out-of-range loop turns the loop OFF rather than being clamped into something the user did
    // not ask for: a silently relocated loop is a different sound, and a sound you cannot explain.
    loopEndClamped_ = std::min(spec_.loopEnd, endFrame_);
    loopLen_ = (spec_.loopMode != 0 && loopEndClamped_ > spec_.loopStart)
                   ? (loopEndClamped_ - spec_.loopStart)
                   : 0;
    looping_ = false;
    loopForward_ = true;
    releasedLoop_ = false;
    // Smoothed values START at their target rather than at zero. A voice that ramps up from
    // silence on every note-on has a 1-block attack nobody asked for, which on a drum is the
    // difference between a kick and a thud.
    gainSmoothed_ = spec_.gain;
    panSmoothed_ = spec_.pan;
    fadeRemaining_ = 0;
    fadeInRemaining_ = 0;
    if (spec_.ampEnv && !spec_.ampEnv->empty()) {
      env_.start(spec_.ampEnv, spec_.envUnitsPerFrame);
      envValue_ = env_.value();
    } else {
      envValue_ = 1.0f;
    }
    released_ = false;
  }

  void release() {
    released_ = true;
    // A SUSTAIN LOOP LETS GO AT NOTE-OFF and plays out to endFrame; a plain loop keeps going.
    // Without this a sustained pad never reaches its own tail and the release is the envelope's
    // alone, which is not what a sample with a recorded decay is for.
    if (spec_.sustainLoop) {
      releasedLoop_ = true;
      // Also stop WRAPPING the interpolator's taps: once the loop is let go the read position
      // leaves the loop range, and taps wrapped back into it would splice unrelated audio onto
      // the tail — a seam fix applied where there is no longer a seam.
      looping_ = false;
    }
    if (spec_.ampEnv && !spec_.ampEnv->empty()) {
      // AT age_, not "now". age_ is the exact frame this note-off landed on; the envelope's own
      // clock only moves at block boundaries, so release() without a frame would start the
      // release wherever the last block ended — making the tail's length depend on the buffer
      // size. The end-to-end determinism check found this after the unit tests were green.
      env_.releaseAt(age_);
    } else {
      // No envelope means no release stage to run, so a gated note ends at note-off. A one-shot
      // slot never calls this at all — that decision belongs to the caller, not here.
      active_ = false;
    }
  }

  // Immediate-ish stop, used by choke and by voice stealing. NEVER an instant cut: a hard stop
  // mid-waveform is a click, and a click is indistinguishable from a bug in the loop points.
  void fadeOut(uint32_t frames) {
    if (!active_) {
      return;
    }
    fadeRemaining_ = frames > 0 ? frames : 1;
    fadeStep_ = 1.0f / static_cast<float>(fadeRemaining_);
  }

  // Ramps IN over `frames`. Used when a voice is STOLEN: the new note starts from a slot that was
  // mid-waveform, so without this the discontinuity at the takeover is a click. Most sounds hide
  // it behind their own attack; an instant-attack drum does not, which is exactly the case where
  // a pool runs out.
  void fadeIn(uint32_t frames) {
    fadeInRemaining_ = frames;
    fadeInTotal_ = frames > 0 ? frames : 1;
  }

  // Peak amplitude this voice is currently contributing. "Steal the quietest" needs a measure,
  // and using the ENVELOPE rather than the last output sample means a voice in a waveform's
  // zero-crossing is not mistaken for a voice that has finished.
  float loudness() const { return active_ ? std::fabs(envValue_) * gainSmoothed_ : 0.0f; }

  // Renders `numFrames` starting at `offsetInBlock` into planar `out`, ADDING (never overwriting —
  // a voice shares its stem with every other voice on it).
  //
  // `offsetInBlock` is what makes voices sample-accurate: a note at sample 100 of the block starts
  // at sample 100, not at 0. That is the property the block-size-invariance check tests, and the
  // reason the built-in beats the hosted-plugin path, which quantises to the block (§3.5).
  void render(float* const* out, uint32_t outChannels, uint32_t offsetInBlock, uint32_t numFrames) {
    if (!active_ || numFrames == 0 || outChannels == 0 || !out) {
      return;
    }
    // THE ENVELOPE IS EVALUATED PER SAMPLE, from a pure function of the frame index.
    //
    // Block-rate evaluation with a linear ramp across the block is what most samplers do and it
    // CANNOT be block-size invariant: the ramp cuts the corner wherever a breakpoint falls inside
    // a block, and where breakpoints fall relative to block boundaries is exactly what changes
    // with block size. EnvRunner::valueAt(f) depends only on the frame index, so blocking is
    // irrelevant by construction rather than by care — and the cursor hint makes the common case
    // one comparison per sample.
    const bool haveEnv = spec_.ampEnv && !spec_.ampEnv->empty();

    // Pan means two different things and conflating them makes a centred stereo clip narrower
    // than the file. Same rule as placed audio clips, deliberately: one meaning of pan per source
    // shape, everywhere in the program.
    const float p = std::clamp(panSmoothed_, -1.0f, 1.0f);
    const bool stereoSource = spec_.source.channels >= 2;
    float gl, gr;
    if (stereoSource) {
      gl = std::min(1.0f, 1.0f - p);  // BALANCE: attenuate a side, never reposition
      gr = std::min(1.0f, 1.0f + p);
    } else {
      const float angle = (p + 1.0f) * 0.25f * static_cast<float>(M_PI);
      gl = std::cos(angle);  // CONSTANT POWER: place a point source
      gr = std::sin(angle);
    }

    const uint64_t stopFrame = spec_.reverse ? startFrame_ : endFrame_;
    for (uint32_t i = 0; i < numFrames; ++i) {
      const uint64_t frame = pos_ >> 32;
      if (!spec_.reverse && frame >= stopFrame) {
        active_ = false;
        break;
      }
      if (spec_.reverse && (pos_ >> 32) < stopFrame) {
        active_ = false;
        break;
      }
      const float t = static_cast<float>(pos_ & 0xFFFFFFFFull) / 4294967296.0f;

      float amp = haveEnv ? env_.valueAt(age_) : 1.0f;
      if (fadeInRemaining_ > 0) {
        // Ramping in after a steal. Counts DOWN, so the gain rises from 0 to 1.
        amp *= 1.0f - static_cast<float>(fadeInRemaining_) / static_cast<float>(fadeInTotal_);
        --fadeInRemaining_;
      }
      if (fadeRemaining_ > 0) {
        amp *= static_cast<float>(fadeRemaining_) * fadeStep_;
        if (--fadeRemaining_ == 0) {
          active_ = false;
        }
      }
      const float g = amp * gainSmoothed_;

      const uint32_t dst = offsetInBlock + i;
      for (uint32_t ch = 0; ch < outChannels && ch < 2; ++ch) {
        const uint32_t srcCh =
            spec_.source.channels >= 2 ? std::min(ch, spec_.source.channels - 1) : 0;
        float s = sampleAt(srcCh, frame, t);
        // LOOP CROSSFADE. Seam-crossing taps make the loop CONTINUOUS in the interpolator's
        // sense — no clamped flat spot — but they cannot make the waveform on either side of
        // the seam MATCH. A sustained tone whose loop points are not at the same phase still
        // steps, and at the loop rate. So the last `loopXfade` frames before the seam are
        // faded against the audio that precedes loopStart, equal-power, which is what makes a
        // hand-set loop on a real recording usable rather than only a lucky one.
        //
        // Forward loops only: ping-pong has no seam to fade (it turns around), and a backward
        // loop's seam is the same one read the other way, which the same blend would smear.
        if (looping_ && !releasedLoop_ && spec_.loopXfade > 0 && spec_.loopMode == 1 &&
            loopLen_ > spec_.loopXfade) {
          const uint64_t fadeFrom = loopEndClamped_ - spec_.loopXfade;
          if (frame >= fadeFrom && frame < loopEndClamped_) {
            const float u = static_cast<float>(frame - fadeFrom) /
                            static_cast<float>(spec_.loopXfade);
            // The audio just BEFORE loopStart is what the loop will jump away from hearing, so
            // it is the natural partner for the tail.
            const uint64_t partner = spec_.loopStart > spec_.loopXfade
                                         ? (spec_.loopStart - spec_.loopXfade +
                                            (frame - fadeFrom))
                                         : (frame - fadeFrom);
            const float other = sampleAt(srcCh, partner, t);
            const float a = std::cos(u * 0.5f * static_cast<float>(M_PI));
            const float b = std::sin(u * 0.5f * static_cast<float>(M_PI));
            s = s * a + other * b;
          }
        }
        out[ch][dst] += s * g * (ch == 0 ? gl : gr);
      }

      // ---- ADVANCE, AND THE THREE LOOP MODES ARE HERE AND NOWHERE ELSE.
      const bool backwards = spec_.reverse != (looping_ && !loopForward_);
      pos_ = backwards ? (pos_ - step_) : (pos_ + step_);
      if (loopLen_ > 0 && !releasedLoop_) {
        const uint64_t a = spec_.loopStart << 32;
        const uint64_t b = loopEndClamped_ << 32;
        if (!looping_) {
          // Not in the loop yet: entering it is what happens the first time playback reaches
          // loopEnd. Before that the slot plays normally from startFrame, which is what makes
          // "attack, then loop" the natural shape rather than a special case.
          if (!spec_.reverse && pos_ >= b) {
            looping_ = true;
          }
        }
        if (looping_) {
          switch (spec_.loopMode) {
            case 2:  // PING-PONG: turn around rather than jump. The seam is continuous, which is
                     // the whole reason to choose it over forward.
              if (loopForward_ && pos_ >= b) {
                pos_ = b - (pos_ - b) - 1;
                loopForward_ = false;
              } else if (!loopForward_ && (pos_ <= a || pos_ > (1ull << 63))) {
                pos_ = a + (a - pos_);
                loopForward_ = true;
              }
              break;
            case 3:  // BACKWARD: the loop runs in reverse, wrapping from start back to end.
              if (loopForward_) {
                loopForward_ = false;  // entered forward; from here the loop runs backwards
                pos_ = b - 1;
              }
              if (pos_ <= a || pos_ > (1ull << 63)) {
                // `while`, not `if`: a loop shorter than one step is not exotic (a 20-frame
                // loop at ratio 4), and a single subtraction would leave the position outside.
                while (pos_ <= a || pos_ > (1ull << 63)) {
                  pos_ += (loopLen_ << 32);
                }
              }
              break;
            default:  // FORWARD
              while (pos_ >= b) {
                pos_ -= (loopLen_ << 32);
              }
              break;
          }
        }
      }
      if (backwards && pos_ > (1ull << 63)) {  // wrapped below zero
        active_ = false;
        break;
      }
      ++age_;
      if (!active_) {
        break;
      }
    }

    // THE DENORMAL / STUCK-VOICE GUARD. An amp envelope decaying toward zero never reaches it, so
    // without this the voice holds a pool slot forever AND runs its arithmetic in the denormal
    // range at ~100x cost.
    //
    // TWO CONDITIONS, and each one alone is a bug:
    //   released_    a HELD note at zero — the bottom of a drawn envelope's dip — is still a live
    //                note and must come back when the envelope rises.
    //   !looping()   a RELEASE-LOOPING note at zero is also going to come back up. Killing it at
    //                the first trough would truncate the loop rather than end it, which is the
    //                opposite failure from the voice leak this guard exists to prevent — the same
    //                envelope, silently wrong in two directions depending on which guard fires
    //                first. A release loop is terminated by EnvShape::releaseFade instead, which
    //                repairEnvShape() guarantees is present whenever a release loop is set.
    if (haveEnv) {
      envValue_ = env_.valueAt(age_);
      env_.advanceTo(age_);
      if (env_.finishedAt(age_)) {
        active_ = false;
      }
    }
    if (released_ && !env_.looping() && std::fabs(envValue_) < kVoiceSilenceFloor) {
      envValue_ = 0.0f;
      active_ = false;
    }
  }

 private:
  // THE INTERPOLATOR READS ACROSS THE SEAM.
  //
  // At a loop boundary the four taps come from the WRAPPED positions rather than from clamped
  // ones. Clamping there flattens the waveform for three samples on every pass, which is a click
  // AT THE LOOP RATE — so it reads as a buzz that is part of the sound rather than as a defect,
  // and it is the single most common reason a looped sampler sounds cheap.
  //
  // At the true start and end of the FILE, clamping remains correct: there is nothing beyond.
  float sampleAt(uint32_t ch, uint64_t frame, float t) const {
    const float* plane = spec_.source.planes[ch];
    const uint64_t last = spec_.source.frames - 1;
    const uint64_t i0 = std::min(frame, last);
    return hermite4(plane[neighbour(i0, -1, last)], plane[i0],
                    plane[neighbour(i0, 1, last)], plane[neighbour(i0, 2, last)], t);
  }

  // Where tap `d` for centre `i0` actually lives. Inside an active loop this wraps into the loop
  // range; outside one it clamps to the file.
  uint64_t neighbour(uint64_t i0, int64_t d, uint64_t last) const {
    int64_t x = static_cast<int64_t>(i0) + d;
    if (looping_ && loopLen_ > 1) {
      const int64_t a = static_cast<int64_t>(spec_.loopStart);
      const int64_t b = static_cast<int64_t>(loopEndClamped_);
      if (x >= b) {
        // Past the seam: continue from the loop's start. Ping-pong turns around instead of
        // wrapping, so its neighbour is mirrored rather than translated.
        x = spec_.loopMode == 2 ? (b - 1 - (x - b)) : (a + (x - b));
      } else if (x < a) {
        x = spec_.loopMode == 2 ? (a + (a - x)) : (b - (a - x));
      }
    }
    if (x < 0) {
      x = 0;
    }
    return std::min<uint64_t>(static_cast<uint64_t>(x), last);
  }

  SamplerVoiceSpec spec_{};
  EnvRunner env_{};
  uint64_t pos_ = 0;  // 32.32 fixed point, in level-0 source frames
  uint64_t step_ = 0;
  uint64_t startFrame_ = 0, endFrame_ = 0;
  uint64_t loopEndClamped_ = 0, loopLen_ = 0;
  bool looping_ = false;      // are we INSIDE the loop right now?
  bool loopForward_ = true;   // ping-pong direction
  bool releasedLoop_ = false; // a sustain loop that has been let go plays out to endFrame
  uint64_t age_ = 0;
  uint32_t noteId_ = 0;
  uint16_t slotId_ = 0;
  uint8_t column_ = 0;
  float envValue_ = 0.0f;
  float gainSmoothed_ = 1.0f;
  float panSmoothed_ = 0.0f;
  uint32_t fadeRemaining_ = 0;
  float fadeStep_ = 0.0f;
  uint32_t fadeInRemaining_ = 0;
  uint32_t fadeInTotal_ = 1;
  bool active_ = false;
  bool released_ = false;
};

}  // namespace daw
