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
// WHAT S1 DOES NOT DO, stated so a half-implementation is not mistaken for a working one:
// no loops (S3 adds forward/ping-pong/backward TOGETHER, with the seam-crossing interpolation and
// the negative control that tells them apart), no mip-map, no filter, no sinc. `loopMode` is not
// read here. A forward loop alone would be ten lines and would be exactly the "backward silently
// aliased to forward" shape this repo keeps deleting.

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
    // Smoothed values START at their target rather than at zero. A voice that ramps up from
    // silence on every note-on has a 1-block attack nobody asked for, which on a drum is the
    // difference between a kick and a thud.
    gainSmoothed_ = spec_.gain;
    panSmoothed_ = spec_.pan;
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
    if (spec_.ampEnv && !spec_.ampEnv->empty()) {
      env_.release();
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
    // ENVELOPE IS BLOCK-RATE AND RAMPED. advance() returns the value at the END of the span so the
    // ramp across it is exact at both ends; per-sample evaluation of a 64-point envelope would
    // cost a segment search per sample and buy nothing audible.
    const float envStart = envValue_;
    float envEnd = envValue_;
    if (spec_.ampEnv && !spec_.ampEnv->empty()) {
      envEnd = env_.advance(numFrames);
      envValue_ = envEnd;
    }
    const float envDelta = (envEnd - envStart) / static_cast<float>(numFrames);

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

      float amp = envStart + envDelta * static_cast<float>(i);
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
        const float s = sampleAt(srcCh, frame, t);
        out[ch][dst] += s * g * (ch == 0 ? gl : gr);
      }

      pos_ = spec_.reverse ? (pos_ - step_) : (pos_ + step_);
      if (spec_.reverse && pos_ > (1ull << 63)) {  // wrapped below zero
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
    if (released_ && !env_.looping() && std::fabs(envValue_) < kVoiceSilenceFloor) {
      envValue_ = 0.0f;
      active_ = false;
    }
    if (spec_.ampEnv && !spec_.ampEnv->empty() && env_.finished()) {
      active_ = false;
    }
  }

 private:
  // Reads with the neighbours the interpolator needs, clamped at the source's edges. S3 replaces
  // the clamping at LOOP boundaries with a wrapped read across the seam — clamping there is where
  // samplers click — but at the true start and end of the file, clamping is correct.
  float sampleAt(uint32_t ch, uint64_t frame, float t) const {
    const float* plane = spec_.source.planes[ch];
    const uint64_t last = spec_.source.frames - 1;
    const uint64_t i0 = std::min(frame, last);
    const uint64_t im1 = i0 > 0 ? i0 - 1 : 0;
    const uint64_t i1 = std::min(i0 + 1, last);
    const uint64_t i2 = std::min(i0 + 2, last);
    return hermite4(plane[im1], plane[i0], plane[i1], plane[i2], t);
  }

  SamplerVoiceSpec spec_{};
  EnvRunner env_{};
  uint64_t pos_ = 0;  // 32.32 fixed point, in level-0 source frames
  uint64_t step_ = 0;
  uint64_t startFrame_ = 0, endFrame_ = 0;
  uint64_t age_ = 0;
  uint32_t noteId_ = 0;
  uint16_t slotId_ = 0;
  uint8_t column_ = 0;
  float envValue_ = 0.0f;
  float gainSmoothed_ = 1.0f;
  float panSmoothed_ = 0.0f;
  uint32_t fadeRemaining_ = 0;
  float fadeStep_ = 0.0f;
  bool active_ = false;
  bool released_ = false;
};

}  // namespace daw
