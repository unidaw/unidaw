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
#include <memory>

#include "apps/sampler_envelope.h"
#include "apps/sampler_mipmap.h"

namespace daw {

// The audio a voice reads. Borrowed, not owned: lifetime belongs to the SamplerRender snapshot
// that the audio thread holds (§3.5), so a voice never touches a refcount.
struct SamplerSourceView {
  const float* const* planes = nullptr;
  uint32_t channels = 0;
  uint64_t frames = 0;
  double sampleRate = 0.0;

  // BAND-LIMITED COPIES for pitching UP (apps/sampler_mipmap.h). levels[L-1] is 1/2^L rate.
  // Absent (mipCount == 0) is legal and means "no anti-aliasing" — which is `Vintage` quality's
  // whole point, and also what a source too short to decimate gets.
  const MipLevel* mips = nullptr;
  uint32_t mipCount = 0;

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
  // 0 Vintage, 1 Fast, 2 Studio. A SOUND, not a setting — see §3. Vintage deliberately skips the
  // mip-map: SP-1200 grit is the point, and an offline render must NEVER quietly upgrade it, or
  // the bounce stops matching what you heard.
  uint8_t quality = 1;
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
  // ENVELOPE CLOCKS, ONE PER ENVELOPE. Not one shared value: every modulator carries its own
  // timeBase (microseconds or nanoticks) and its own rate multiplier, so a tempo-synced filter
  // sweep under a millisecond-timed amp envelope is an ordinary thing to ask for.
  //
  // This WAS one field, set only inside the `if (amp)` branch — so a cutoff, pitch or pan
  // envelope on a mod set with no amp envelope ran with a clock of ZERO, sat at its value at
  // time 0 forever, and modulated nothing. The command that created it worked, the modulator
  // persisted correctly, and the sound was identical to having no envelope at all.
  double envUnitsPerFrame = 0.0;
  double cutoffUnitsPerFrame = 0.0;
  double pitchUnitsPerFrame = 0.0;
  double panUnitsPerFrame = 0.0;
  double resonanceUnitsPerFrame = 0.0;

  // FILTER, and the modulators that move it. All envelopes are borrowed from the snapshot, so a
  // voice never owns one and never frees one.
  uint8_t filterType = 0;      // 0 off, 1 LP12, 2 LP24, 3 HP, 4 BP
  float cutoffHz = 20000.0f;
  float resonance = 0.7f;
  // VINTAGE, applied BEFORE the filter — the order the machines this imitates actually had, and
  // the reason 12-bit sounds like 12-bit rather than like noise added to a filtered signal.
  // 0 disables each independently.
  uint8_t bitDepth = 0;        // 1..16; amplitude quantised to 2^n levels
  uint32_t holdFrames = 0;     // sample-and-hold length, derived from the target rate
  const EnvShape* cutoffEnv = nullptr;
  float cutoffDepth = 0.0f;    // in octaves at full envelope
  const EnvShape* pitchEnv = nullptr;
  float pitchDepthCents = 0.0f;
  const EnvShape* panEnv = nullptr;
  float panDepth = 0.0f;
  const EnvShape* resonanceEnv = nullptr;
  float resonanceDepth = 0.0f;  // in Q units at full envelope; the filter's range is 0.7..10

  // ---- LFOs. ModKind::Lfo was in the model and in the file format from the start, and NOTHING
  // rendered it: a modulator kind that saved, loaded, and made no sound. This is it.
  //
  // NOTE-RETRIGGERED, not free-running, and that is a decision. The phase is a pure function of
  // the voice's age, exactly like EnvRunner — so two notes at the same tick sound identical
  // whatever the transport did before them, and the value at frame f does not depend on where
  // the block boundaries fell. A timeline-locked LFO is the right thing for a patcher control
  // signal and the wrong thing inside a voice, where it would make a render depend on when
  // playback started. `phaseOffset` is how you move it deliberately.
  struct VoiceLfo {
    float cyclesPerFrame = 0.0f;
    float phase0 = 0.0f;  // in turns
    float amp = 0.0f;     // the LFO's own depth, already scaled into TARGET units
    float bias = 0.0f;    // likewise, so the voice does no unit conversion at all
    bool active = false;
  };
  VoiceLfo volLfo, panLfo, pitchLfo, cutoffLfo, resLfo;
  double sampleRate = 48000.0;
};

// -120 dBFS. An amp envelope below this is inaudible, and a voice that stays there is two bugs at
// once: it holds a pool slot forever, and its arithmetic runs in the denormal range at roughly
// 100x the cost of normal floats (§3.5 — nothing else in this repo handles denormals). Ending the
// voice fixes both, and it is the cheaper of the two available fixes.
inline constexpr float kVoiceSilenceFloor = 1.0e-6f;

// THREE INTERPOLATORS, BECAUSE QUALITY IS A SOUND AND NOT A SETTING.
//
// All three fix pitching DOWN. NONE of them help pitching UP — that is the mip-map's job, and
// conflating the two is the single most common sampler mistake.
//
//   Vintage  linear. -6 dB/oct image rejection, audibly gritty on pitched material, and paired
//            with skipping the mip-map entirely. SP-1200 / S950 character, chosen on purpose.
//   Fast     4-point cubic Hermite. No ringing, cheap, right for drums and slices. The default.
//   Studio   16-tap windowed sinc. For melodic multisamples and long downward transposition,
//            where Hermite's gentle high-end roll-off is audible as dullness.
//
// AN OFFLINE RENDER MUST NEVER UPGRADE THIS. A quality setting silently improved at bounce time
// makes the render not match what you heard, which is exactly the class of divergence this
// codebase spends its effort removing.

// Linear. Deliberately the crude one.
inline float lerpSample(float y0, float y1, float t) { return y0 + (y1 - y0) * t; }

// 4-point cubic Hermite.
inline float hermite4(float ym1, float y0, float y1, float y2, float t) {
  const float c0 = y0;
  const float c1 = 0.5f * (y1 - ym1);
  const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
  return ((c3 * t + c2) * t + c1) * t + c0;
}

inline constexpr int kSincTaps = 16;
inline constexpr int kSincPhases = 128;

// The sinc table, built ONCE at first use and never on the audio thread. A Kaiser window at
// beta = 8 puts the stopband around -90 dB, matching the mip-map's filter so neither is the
// weak link.
//
// PHASES ARE INTERPOLATED BETWEEN, not rounded to. 128 phases alone would quantise the read
// position to 1/128 of a sample, which is its own (quiet, broadband) distortion — and a table
// fine enough to skip that step would be 100x larger for no audible gain.
struct SincTable {
  float taps[kSincPhases + 1][kSincTaps]{};
  SincTable() {
    const double beta = 8.0;
    const double denom = besselI0(beta);
    for (int p = 0; p <= kSincPhases; ++p) {
      const double frac = static_cast<double>(p) / static_cast<double>(kSincPhases);
      for (int k = 0; k < kSincTaps; ++k) {
        const double x = static_cast<double>(k - kSincTaps / 2 + 1) - frac;
        const double sinc = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double r = x / static_cast<double>(kSincTaps / 2);
        const double w = (std::fabs(r) >= 1.0)
                             ? 0.0
                             : besselI0(beta * std::sqrt(1.0 - r * r)) / denom;
        taps[p][k] = static_cast<float>(sinc * w);
      }
    }
  }
};

inline const SincTable& sincTable() {
  static const SincTable t;
  return t;
}

// A TPT (topology-preserving-transform) state-variable filter. One structure yields low-pass,
// high-pass and band-pass from the same two integrators, and — unlike a biquad with recomputed
// coefficients — it stays stable when the cutoff is swept FAST, which is exactly what a filter
// envelope does. A biquad modulated per sample can and does blow up; this cannot.
//
// LP24 is two of these in series rather than a different filter, so "steeper" means what it says.
struct SvfFilter {
  float ic1 = 0.0f, ic2 = 0.0f;

  void reset() { ic1 = ic2 = 0.0f; }

  // `cutoffHz` and `q` are per SAMPLE: the whole point is that they can move.
  float process(float x, float cutoffHz, float q, uint8_t type, double sampleRate) {
    const float fc = std::clamp(cutoffHz, 20.0f, static_cast<float>(sampleRate * 0.45));
    const float g = std::tan(static_cast<float>(M_PI) * fc / static_cast<float>(sampleRate));
    const float k = 1.0f / std::max(0.05f, q);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + g * v1;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    switch (type) {
      case 3: return x - k * v1 - v2;  // high-pass
      case 4: return v1;               // band-pass
      default: return v2;              // low-pass (12 and the first stage of 24)
    }
  }
};

class SamplerVoice {
 public:
  bool active() const { return active_; }
  uint32_t noteId() const { return noteId_; }
  uint8_t column() const { return column_; }
  uint16_t slotId() const { return slotId_; }
  // Frames rendered since note-on. The tiebreak for "steal the oldest" — a musical quantity, not
  // wall-clock, so a bounce steals the same voice the audition did (§3.5 determinism).
  uint64_t age() const { return age_; }

  // `pin` is the snapshot spec's pointers refer into. Passed as shared_ptr<const void> so this
  // header does not need SamplerRender's definition — the voice never dereferences it, it only
  // keeps it alive.
  void start(const SamplerVoiceSpec& spec,
             uint32_t noteId,
             uint16_t slotId,
             uint8_t column,
             std::shared_ptr<const void> pin = nullptr) {
    spec_ = spec;
    snapshotPin_ = std::move(pin);
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
    // WHICH BAND-LIMITED LEVEL. Chosen once at note-on from the playback ratio; S3 fixes the
    // ratio for the life of the voice, and a pitch envelope (which would move it per block) is
    // the reason mipFrac() derives everything from the level-0 position rather than caching a
    // per-level cursor.
    //
    // `Vintage` quality skips the mip-map ENTIRELY and that is the whole point of it: SP-1200
    // grit is aliasing, deliberately. The offline render must never quietly upgrade it.
    if (spec_.quality == 0 || spec_.source.mipCount == 0) {
      mipLevel_ = 0;
      mipBlend_ = 0.0f;
    } else {
      mipLevel_ = mipLevelFor(spec_.ratio, spec_.source.mipCount);
      mipBlend_ = mipBlend(spec_.ratio, mipLevel_);
    }
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
    // The other modulators each run their OWN clock. They are separate RUNNERS rather than one
    // runner read at three depths, because they are separate SHAPES — a filter that opens while
    // the amp decays is the ordinary case, not the exotic one.
    //
    // A CLOCK OF ZERO MEANS UNSET, NOT FROZEN, and that distinction is the whole reason this
    // helper exists. Splitting one shared clock into five fixed the original bug — a cutoff
    // envelope on a mod set with no amp envelope ran at zero and modulated nothing — and
    // reintroduced exactly the same failure one layer out: every caller written against the old
    // single-field API sets envUnitsPerFrame and nothing else, so its cutoff envelope went
    // silently dead. sampler_dsp_tests is such a caller and it caught this, four hours late,
    // because the test binary itself had not been rebuilt.
    //
    // Zero is safe to overload because an envelope that never advances is not a configuration
    // anyone wants: it sits at its value at time 0 forever, which is indistinguishable from
    // having no envelope at all. So zero can only ever mean "nobody filled this in", and the
    // honest response is to fall back to the voice's master clock rather than to run dead.
    const auto clockFor = [&](double own) { return own > 0.0 ? own : spec_.envUnitsPerFrame; };
    if (spec_.cutoffEnv && !spec_.cutoffEnv->empty()) {
      cutoffEnv_.start(spec_.cutoffEnv, clockFor(spec_.cutoffUnitsPerFrame));
    }
    if (spec_.pitchEnv && !spec_.pitchEnv->empty()) {
      pitchEnv_.start(spec_.pitchEnv, clockFor(spec_.pitchUnitsPerFrame));
    }
    if (spec_.panEnv && !spec_.panEnv->empty()) {
      panEnv_.start(spec_.panEnv, clockFor(spec_.panUnitsPerFrame));
    }
    if (spec_.resonanceEnv && !spec_.resonanceEnv->empty()) {
      resonanceEnv_.start(spec_.resonanceEnv, clockFor(spec_.resonanceUnitsPerFrame));
    }
    filtL_.reset();
    filtR_.reset();
    filtL2_.reset();
    filtR2_.reset();
    baseStep_ = step_;
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
      cutoffEnv_.releaseAt(age_);
      pitchEnv_.releaseAt(age_);
      panEnv_.releaseAt(age_);
      resonanceEnv_.releaseAt(age_);
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
    auto panGains = [stereoSource](float pan, float& l, float& r) {
      if (stereoSource) {
        l = std::min(1.0f, 1.0f - pan);  // BALANCE: attenuate a side, never reposition
        r = std::min(1.0f, 1.0f + pan);
      } else {
        const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(M_PI);
        l = std::cos(angle);  // CONSTANT POWER: place a point source
        r = std::sin(angle);
      }
    };
    float gl, gr;
    panGains(p, gl, gr);
    // A PAN ENVELOPE MOVES THE SOUND, so when one is running the gains are recomputed PER
    // SAMPLE. They used to be computed once per block and the envelope was started, released
    // and never evaluated at all — spec.panDepth was set and never read, so a Panning envelope
    // was a modulator the document promised and the sound never had.
    //
    // Per block would also make the output depend on where the block boundaries fall, which is
    // the determinism failure the pitch envelope's comment already argues against.
    const bool panMoving =
        (spec_.panEnv && !spec_.panEnv->empty() && spec_.panDepth != 0.0f) ||
        spec_.panLfo.active;

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
      float g = amp * gainSmoothed_;
      if (spec_.volLfo.active) {
        // TREMOLO. Multiplies, like every other Volume modulator: 1 + v, so a depth of 1 swings
        // between silence and double and a depth of 0.3 is the gentle thing people actually use.
        g *= std::clamp(1.0f + lfoAt(spec_.volLfo, age_), 0.0f, 2.0f);
      }

      // ---- PITCH MODULATION, per sample, and the MIP LEVEL FOLLOWS IT.
      //
      // The level is re-chosen from the CURRENT ratio rather than fixed at note-on, because a
      // pitch envelope sweeping an octave is exactly what the mip-map exists for. It is done
      // per SAMPLE, not per block: choosing it per block would make the output depend on where
      // the block boundaries fall, which is the determinism failure this whole design is built
      // to avoid. It costs comparisons rather than a log2 — the thresholds are powers of two.
      if ((spec_.pitchEnv && !spec_.pitchEnv->empty() && spec_.pitchDepthCents != 0.0f) ||
          spec_.pitchLfo.active) {
        float cents = spec_.pitchLfo.active ? lfoAt(spec_.pitchLfo, age_) : 0.0f;
        if (spec_.pitchEnv && !spec_.pitchEnv->empty() && spec_.pitchDepthCents != 0.0f) {
          cents += pitchEnv_.valueAt(age_) * spec_.pitchDepthCents;
        }
        const double mul = std::pow(2.0, static_cast<double>(cents) / 1200.0);
        step_ = static_cast<uint64_t>(static_cast<double>(baseStep_) * mul);
        if (spec_.quality != 0 && spec_.source.mipCount > 0) {
          const double ratio = spec_.ratio * mul;
          uint32_t lvl = 0;
          while (lvl < spec_.source.mipCount && ratio >= std::pow(2.0, lvl + 1.0)) {
            ++lvl;
          }
          mipLevel_ = lvl;
          mipBlend_ = mipBlend(ratio, lvl);
        }
      }

      float glNow = gl, grNow = gr;
      if (panMoving) {
        float pan = p + lfoAt(spec_.panLfo, age_);
        if (spec_.panEnv && !spec_.panEnv->empty()) {
          pan += panEnv_.valueAt(age_) * spec_.panDepth;
        }
        panGains(std::clamp(pan, -1.0f, 1.0f), glNow, grNow);
      }

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
        // ---- VINTAGE, BEFORE THE FILTER. Rate first, then bits: the converter's word length
        // applies to whatever the sample-and-hold last latched, which is what the hardware did
        // and what makes the two controls interact the way people expect.
        //
        // THE HOLD COUNTER IS PER VOICE AND STARTS AT ZERO ON NOTE-ON. Derived from a global
        // sample counter it would make one note sound different depending on where the transport
        // happened to be, and two bounces of one project would differ — the same rule the
        // envelope clock follows, and the one property this whole subsystem must not break.
        if (spec_.holdFrames > 1) {
          if (holdCount_[ch] == 0) {
            held_[ch] = s;
          }
          s = held_[ch];
        }
        if (spec_.bitDepth > 0 && spec_.bitDepth < 16) {
          // 2^(n-1) steps either side of zero. Rounding rather than truncating, so silence stays
          // silence: truncation biases every sample toward zero and turns a decaying tail into a
          // DC step.
          const float levels = static_cast<float>(1u << (spec_.bitDepth - 1));
          s = std::round(std::clamp(s, -1.0f, 1.0f) * levels) / levels;
        }
        // ---- THE FILTER, per sample so its envelope can actually move it. Off by default:
        // filtering a drum kit that asked for none is a sound nobody chose.
        if (spec_.filterType != 0) {
          float fc = spec_.cutoffHz;
          if (spec_.cutoffLfo.active) {
            fc *= std::pow(2.0f, lfoAt(spec_.cutoffLfo, age_));  // the wobble
          }
          if (spec_.cutoffEnv && !spec_.cutoffEnv->empty() && spec_.cutoffDepth != 0.0f) {
            // Depth is in OCTAVES, not hertz. A filter envelope that moved a fixed number of
            // hertz would be a different musical gesture at every cutoff setting — barely
            // audible when open, a slam when closed.
            fc *= std::pow(2.0f, cutoffEnv_.valueAt(age_) * spec_.cutoffDepth);
          }
          // RESONANCE MODULATION. ModTarget::Resonance was in the enum and fell through the
          // engine's switch, so it was a target you could name and nothing would happen.
          float q = spec_.resonance;
          if (spec_.resonanceEnv && !spec_.resonanceEnv->empty() &&
              spec_.resonanceDepth != 0.0f) {
            q += resonanceEnv_.valueAt(age_) * spec_.resonanceDepth;
          }
          if (spec_.resLfo.active) {
            q += lfoAt(spec_.resLfo, age_);
          }
          q = std::clamp(q, 0.7f, 10.0f);
          SvfFilter& f1 = (ch == 0) ? filtL_ : filtR_;
          s = f1.process(s, fc, q, spec_.filterType, spec_.sampleRate);
          if (spec_.filterType == 2) {  // LP24 is two LP12s, so "steeper" means what it says
            SvfFilter& f2 = (ch == 0) ? filtL2_ : filtR2_;
            s = f2.process(s, fc, q, 1, spec_.sampleRate);
          }
        }
        out[ch][dst] += s * g * (ch == 0 ? glNow : grNow);
        if (spec_.holdFrames > 1) {
          holdCount_[ch] = (holdCount_[ch] + 1) % spec_.holdFrames;
        }
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
    // LEVEL 0, or a band-limited level when pitching up. The level-L index is `frame >> L`,
    // EXACT by construction, which is the entire reason position is 32.32 in level-0 frames
    // rather than a float per level: a glide sweeping an octave boundary does not jump.
    if (mipLevel_ == 0) {
      return readLevel(spec_.source.planes, spec_.source.frames, 0, ch, frame, t);
    }
    const MipLevel& lo = spec_.source.mips[mipLevel_ - 1];
    const float a = readLevel(lo.planes.data(), lo.frames, mipLevel_, ch,
                              frame >> mipLevel_, mipFrac(frame, t, mipLevel_));
    if (mipBlend_ <= 0.0f || mipLevel_ >= spec_.source.mipCount) {
      return a;
    }
    // CROSSING the boundary rather than stepping over it. A static note at a boundary sounds
    // correct on either level; a SWEEP through one does not, and sweeps are what pitch envelopes
    // and glides do.
    const MipLevel& hi = spec_.source.mips[mipLevel_];
    const uint32_t L = mipLevel_ + 1;
    const float b = readLevel(hi.planes.data(), hi.frames, L, ch, frame >> L, mipFrac(frame, t, L));
    return a * (1.0f - mipBlend_) + b * mipBlend_;
  }

  // The fractional part of a level-L read position, DERIVED from the level-0 position so the two
  // can never disagree. Shifting the integer part and recomputing the fraction independently is
  // how a mip-mapped sampler ends up detuned per level.
  static float mipFrac(uint64_t frame, float t, uint32_t level) {
    const uint64_t mask = (1ull << level) - 1;
    return (static_cast<float>(frame & mask) + t) / static_cast<float>(1ull << level);
  }

  // `level` is passed rather than inferred from the frame count: two levels of a short sample can
  // round to the same length, and inferring would then read one while wrapping for the other.
  float readLevel(const float* const* planes, uint64_t frames, uint32_t level, uint32_t ch,
                  uint64_t frame, float t) const {
    if (!planes || frames == 0) {
      return 0.0f;
    }
    const float* plane = planes[std::min<uint32_t>(ch, spec_.source.channels - 1)];
    const uint64_t last = frames - 1;
    const uint64_t i0 = std::min(frame, last);
    switch (spec_.quality) {
      case 0:  // Vintage — linear, and the grit is the point
        return lerpSample(plane[i0], plane[neighbourAt(i0, 1, last, level)], t);
      case 2: {  // Studio — 16-tap windowed sinc
        const SincTable& tbl = sincTable();
        const float fp = t * static_cast<float>(kSincPhases);
        const int p = std::min(static_cast<int>(fp), kSincPhases - 1);
        const float pf = fp - static_cast<float>(p);
        float acc = 0.0f;
        for (int k = 0; k < kSincTaps; ++k) {
          const int64_t d = static_cast<int64_t>(k) - (kSincTaps / 2 - 1);
          const float x = plane[neighbourAt(i0, d, last, level)];
          // Blend BETWEEN phases rather than rounding to one: 128 phases alone quantise the read
          // position to 1/128 of a sample, which is its own quiet broadband distortion.
          acc += x * (tbl.taps[p][k] * (1.0f - pf) + tbl.taps[p + 1][k] * pf);
        }
        return acc;
      }
      default:  // Fast — Hermite
        return hermite4(plane[neighbourAt(i0, -1, last, level)], plane[i0],
                        plane[neighbourAt(i0, 1, last, level)],
                        plane[neighbourAt(i0, 2, last, level)], t);
    }
  }

  // Where tap `d` for centre `i0` lives at this level. Inside an active loop it wraps into the
  // loop range; outside one it clamps to the file.
  //
  // THE LOOP RANGE IS SHIFTED FROM LEVEL-0 FRAMES, never recomputed per level. A loop whose
  // bounds are derived independently at each level drifts against the read position, and the
  // loop DETUNES as the note rises — a real bug in real samplers, and one that only shows up on
  // sustained material transposed far.
  uint64_t neighbourAt(uint64_t i0, int64_t d, uint64_t last, uint32_t level) const {
    int64_t x = static_cast<int64_t>(i0) + d;
    if (looping_ && loopLen_ > 1) {
      const int64_t a = static_cast<int64_t>(spec_.loopStart >> level);
      const int64_t b = static_cast<int64_t>(loopEndClamped_ >> level);
      if (b > a) {
        if (x >= b) {
          // Ping-pong turns around, so its neighbour is MIRRORED rather than translated — that
          // is what its seam actually is.
          x = spec_.loopMode == 2 ? (b - 1 - (x - b)) : (a + (x - b));
        } else if (x < a) {
          x = spec_.loopMode == 2 ? (a + (a - x)) : (b - (a - x));
        }
      }
    }
    if (x < 0) {
      x = 0;
    }
    return std::min<uint64_t>(static_cast<uint64_t>(x), last);
  }

  SamplerVoiceSpec spec_{};
  EnvRunner env_{};
  EnvRunner cutoffEnv_{}, pitchEnv_{}, panEnv_{}, resonanceEnv_{};

  // sin over TURNS, so the phase arithmetic stays in the same units the config uses.
  static float lfoAt(const SamplerVoiceSpec::VoiceLfo& l, uint64_t frame) {
    if (!l.active) {
      return 0.0f;
    }
    const float turns = l.phase0 + l.cyclesPerFrame * static_cast<float>(frame);
    return std::sin(turns * 2.0f * static_cast<float>(M_PI)) * l.amp + l.bias;
  }
  SvfFilter filtL_{}, filtR_{}, filtL2_{}, filtR2_{};
  // Per channel, so a stereo sample's two sides hold together rather than drifting a frame
  // apart — which would widen the image as a side effect of a mono tone control.
  uint32_t holdCount_[2]{0, 0};
  float held_[2]{0.0f, 0.0f};
  uint64_t pos_ = 0;  // 32.32 fixed point, in level-0 source frames
  uint64_t step_ = 0;
  uint64_t startFrame_ = 0, endFrame_ = 0;
  uint64_t baseStep_ = 0;  // the unmodulated increment; the pitch envelope scales this
  uint64_t loopEndClamped_ = 0, loopLen_ = 0;
  bool looping_ = false;      // are we INSIDE the loop right now?
  bool loopForward_ = true;   // ping-pong direction
  bool releasedLoop_ = false; // a sustain loop that has been let go plays out to endFrame
  uint32_t mipLevel_ = 0;     // which band-limited level this voice reads
  float mipBlend_ = 0.0f;     // how much of level+1 to blend in across the boundary
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
  // THE SNAPSHOT THIS VOICE IS READING, held for as long as it sounds.
  //
  // Every pointer in spec_ — the envelopes, the decoded planes, the mip-map — points INTO a
  // SamplerRender this voice does not own. Holding a reference is what stops that render being
  // freed underneath a sounding note when an edit lands (see SamplerEngine::setSnapshot). It is
  // a shared_ptr and not a raw copy because the data is large and shared by every voice.
  //
  // Assigned on the audio thread, which is safe: copying a shared_ptr is an atomic increment,
  // and the DEcrement here never reaches zero because the engine's retire list holds one until
  // it can see that no voice does. Nothing is allocated and nothing is freed on this thread.
  std::shared_ptr<const void> snapshotPin_;
};

}  // namespace daw
