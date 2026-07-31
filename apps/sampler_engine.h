#pragma once

// THE POLYPHONIC LAYER: a voice pool, note dispatch, choke, NNA and stealing.
//
// Between the document (apps/sampler_state.h) and one voice (apps/sampler_voice.h). It still knows
// nothing about the engine, SHM or a device chain — it takes a snapshot, a list of note events
// with sample offsets, and a planar output buffer.
//
// THE BLOCK IS SEGMENTED AT EVENT BOUNDARIES. Events are sorted by offset and the block is
// rendered in runs between them: [0, e0), apply e0, [e0, e1), and so on. That is the only shape
// that makes BOTH note-on and note-off sample-accurate — starting a voice at an offset is easy,
// but RELEASING one at an offset is not expressible any other way, and a note-off quantised to the
// block boundary makes a short gated note a different length depending on the buffer size.
//
// NO ALLOCATION ANYWHERE. The pool is sized once; the event list is the caller's; the segment walk
// uses no container at all.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "apps/sampler_slice.h"
#include "apps/sampler_state.h"
#include "apps/sampler_voice.h"

namespace daw {

// A decoded source held by the snapshot. shared_ptr so the audio thread holding a snapshot keeps
// the audio alive BY CONSTRUCTION, and the last reference dies on the command thread when the
// snapshot is replaced — §3.5, and the reason "the audio thread never frees" is a property of
// where the reference lives rather than a rule anyone has to remember.
struct SamplerSourceAudio {
  std::vector<std::vector<float>> channels;  // planar
  std::vector<const float*> planes;          // pointers into `channels`, built once
  uint64_t frames = 0;
  double sampleRate = 0.0;

  void buildPlanes() {
    planes.clear();
    planes.reserve(channels.size());
    for (const auto& c : channels) {
      planes.push_back(c.data());
    }
  }
  SamplerSourceView view() const {
    SamplerSourceView v;
    v.planes = planes.data();
    v.channels = static_cast<uint32_t>(planes.size());
    v.frames = frames;
    v.sampleRate = sampleRate;
    return v;
  }
};

// What the audio thread reads. Immutable once published: built on the command thread, swapped in
// by atomic_store_explicit, never mutated in place.
struct SamplerRender {
  SamplerState state;  // a COPY, so an edit to the document cannot be seen half-applied
  SamplerKeymap keymap;
  std::vector<std::shared_ptr<const SamplerSourceAudio>> audio;  // parallel to state.sources
  double sampleRate = 48000.0;

  const SamplerSourceAudio* audioFor(uint16_t sourceLocalId) const {
    for (size_t i = 0; i < state.sources.size() && i < audio.size(); ++i) {
      if (state.sources[i].localId == sourceLocalId) {
        return audio[i].get();
      }
    }
    return nullptr;
  }
};

enum class SamplerEventKind : uint8_t { NoteOn = 0, NoteOff = 1, AllOff = 2 };

struct SamplerEvent {
  uint32_t offsetInBlock = 0;
  SamplerEventKind kind = SamplerEventKind::NoteOn;
  uint8_t pitch = 0;
  uint8_t velocity = 0;
  uint8_t column = 0;
  uint16_t sound = 0;   // 0 = resolve through the keymap (R2)
  uint32_t noteId = 0;  // the engine's voice id; identifies this note for note-off
  uint16_t offsetFrac = 0;  // the 9xx seek, as a fraction of the slot's extent (0 = from the start)
};

// 2 ms. Long enough that a takeover is not a click, short enough that the new note is not late.
inline constexpr double kStealFadeMs = 2.0;

class SamplerRuntime {
 public:
  // Sized once, off the audio thread, and EXACTLY voiceCap.
  //
  // An earlier version padded the pool with a reserve so a stolen voice had somewhere to fade out.
  // That made voiceCap a lie — asking for 4 voices got 12 — which is worse than the problem it
  // solved: a voice cap that is not the voice count cannot be reasoned about, and it silently
  // raised the CPU ceiling the setting exists to bound. Stealing instead REUSES the victim's slot
  // and ramps the new note in over kStealFadeMs, which is what actually removes the click; the
  // victim's tail is lost, which is the honest cost of a pool that has run out.
  void configure(uint8_t voiceCap, double sampleRate) {
    sampleRate_ = sampleRate;
    voices_.resize(voiceCap > 0 ? voiceCap : 1);
    cap_ = voiceCap;
  }

  // A RETIRED SNAPSHOT IS NOT FREED WHILE A VOICE IS STILL READING IT.
  //
  // This used to be `snap_ = std::move(snap)`, which dropped the last reference to the outgoing
  // SamplerRender and freed it — while sounding voices held RAW pointers into its envelopes,
  // its decoded sample planes and its mip-map. SamplerVoice says so in as many words: "All
  // envelopes are borrowed from the snapshot, so a voice never owns one and never frees one."
  // Every sampler edit goes through refreshSamplerForTrack, so ANY edit during playback — a
  // filter change, an envelope tweak, a slice drag — could take the engine down. AddressSanitizer
  // called it: heap-use-after-free, READ of size 8, on a render-pool worker. About one run in ten
  // from a single edit and 3/3 under a hammer, which is why it survived this long.
  //
  // WHY use_count() IS SOUND HERE, since it usually is not. Only this thread ever ADDS a
  // reference to a retired snapshot: a new voice always pins snap_, never a retired one, so a
  // retired entry's count can only fall. `1` therefore means "the retire list is the last
  // holder" and cannot become 2 behind our back. Voices decrement from the audio thread, which
  // is a refcount atomic and frees nothing, because this list is still holding one.
  //
  // Collected here rather than on a timer: a sustained note can hold its snapshot for as long as
  // it sounds, so any grace period short enough to bound memory is short enough to be wrong.
  void setSnapshot(std::shared_ptr<const SamplerRender> snap) {
    if (snap_ && snap_ != snap) {
      retired_.push_back(std::move(snap_));
    }
    snap_ = std::move(snap);
    for (size_t i = retired_.size(); i-- > 0;) {
      if (retired_[i].use_count() == 1) {
        retired_[i] = std::move(retired_.back());
        retired_.pop_back();
      }
    }
  }
  const SamplerRender* snapshot() const { return snap_.get(); }
  // Snapshots kept alive only because a voice is still sounding from them. Telemetry: a number
  // that climbs and never falls means the collection rule above has stopped working.
  size_t retiredSnapshots() const { return retired_.size(); }

  uint32_t activeVoices() const {
    uint32_t n = 0;
    for (const auto& v : voices_) {
      if (v.active()) {
        ++n;
      }
    }
    return n;
  }
  // Counts note-ons that had to steal a sounding voice. Published as telemetry rather than left
  // to be noticed as "the roll sounds wrong" — a pool running out is a musical fact the user
  // should be told about, not a silent truncation.
  uint64_t stealCount() const { return steals_; }
  uint64_t unmappedCount() const { return unmapped_; }

  // Renders one block into MAIN out, plus optional per-STEM stereo pairs.
  //
  // `stemPlanes` is numStems*2 channels: stem N (1-based) writes to stemPlanes[(N-1)*2] and +1.
  // A slot with outputStem == 0 goes to the main output, which is the ordinary case; a slot with
  // a stem goes THERE INSTEAD, not as well. Sending it to both would double it in the master the
  // moment its child track was unmuted, which is a bug you only hear as "the kick is loud".
  void render(float* const* out,
              uint32_t outChannels,
              uint32_t numFrames,
              const SamplerEvent* events,
              uint32_t numEvents,
              float* const* stemPlanes = nullptr,
              uint32_t stemCount = 0) {
    stemPlanes_ = stemPlanes;
    stemCount_ = stemCount;
    if (!out || outChannels == 0 || numFrames == 0) {
      return;
    }
    uint32_t cursor = 0;
    uint32_t ei = 0;
    while (cursor < numFrames) {
      // How far can we render before the next event? Everything at THIS offset is applied first,
      // so several events landing on one sample all take effect before any audio is produced.
      uint32_t next = numFrames;
      while (ei < numEvents && events[ei].offsetInBlock <= cursor) {
        apply(events[ei]);
        ++ei;
      }
      if (ei < numEvents) {
        next = std::min(numFrames, events[ei].offsetInBlock);
      }
      const uint32_t span = next - cursor;
      if (span > 0) {
        for (auto& v : voices_) {
          if (!v.active()) {
            continue;
          }
          const SamplerSlot* slot = snap_ ? snap_->state.findSlot(v.slotId()) : nullptr;
          const uint8_t stem = slot ? slot->outputStem : 0;
          if (stem > 0 && stemPlanes_ && stem <= stemCount_) {
            float* pair[2] = {stemPlanes_[(stem - 1) * 2], stemPlanes_[(stem - 1) * 2 + 1]};
            v.render(pair, 2, cursor, span);
          } else {
            v.render(out, outChannels, cursor, span);
          }
        }
        cursor = next;
      } else if (ei >= numEvents) {
        break;
      }
    }
  }

  void allNotesOff() {
    for (auto& v : voices_) {
      if (v.active()) {
        v.fadeOut(fadeFrames(kStealFadeMs));
      }
    }
  }

 private:
  uint32_t fadeFrames(double ms) const {
    const uint32_t n = static_cast<uint32_t>(sampleRate_ * ms / 1000.0);
    return n > 0 ? n : 1;
  }

  void apply(const SamplerEvent& e) {
    switch (e.kind) {
      case SamplerEventKind::AllOff:
        allNotesOff();
        return;
      case SamplerEventKind::NoteOff:
        noteOff(e);
        return;
      case SamplerEventKind::NoteOn:
        noteOn(e);
        return;
    }
  }

  void noteOff(const SamplerEvent& e) {
    for (auto& v : voices_) {
      if (!v.active() || v.noteId() != e.noteId) {
        continue;
      }
      const SamplerSlot* slot = snap_ ? snap_->state.findSlot(v.slotId()) : nullptr;
      // A ONE-SHOT IGNORES NOTE-OFF. That is the difference between a drum and a pad, and it is
      // the slot's decision — the tracker's authored OFF still arrives, it just does not apply.
      if (slot && slot->gate == 0) {
        continue;
      }
      v.release();
    }
  }

  void noteOn(const SamplerEvent& e) {
    if (!snap_) {
      return;
    }
    const SamplerState& st = snap_->state;
    // R2's resolution rule, in the one place it is implemented:
    //   sound != 0 -> that slot, and pitch is varispeed relative to its rootKey
    //   sound == 0 -> the keymap, and pitch means exactly the same thing
    uint16_t slotId = e.sound;
    if (slotId == 0) {
      slotId = resolveSlot(st, snap_->keymap, e.pitch, e.velocity, rrCounter_[e.pitch & 127]++);
    }
    const SamplerSlot* slot = st.findSlot(slotId);
    if (!slot) {
      // Nothing mapped here. Counted, not logged — this runs on the producer thread and a note
      // into an empty key is a normal authoring state, not an error, but a kit that is silent
      // everywhere should be diagnosable without a debugger.
      ++unmapped_;
      return;
    }
    const SamplerSourceAudio* audio = snap_->audioFor(slot->sourceLocalId);
    if (!audio || audio->frames == 0) {
      ++unmapped_;
      return;
    }

    // 1. VOICE GROUP CHOKE. Open hat / closed hat is two fields, not a container.
    if (slot->voiceGroup != 0) {
      for (auto& v : voices_) {
        if (!v.active()) {
          continue;
        }
        const SamplerSlot* other = st.findSlot(v.slotId());
        if (other && other->voiceGroup == slot->voiceGroup) {
          v.fadeOut(fadeFrames(static_cast<double>(slot->chokeFadeUs) / 1000.0));
        }
      }
    }

    // 2. NNA against the previous voice from the same (column, slot). IT's answer and the
    //    tracker-native one: a voice rule, not a bookkeeping table.
    for (auto& v : voices_) {
      if (!v.active() || v.column() != e.column || v.slotId() != slotId) {
        continue;
      }
      switch (slot->nna) {
        case SamplerNna::Cut:
          v.fadeOut(fadeFrames(kStealFadeMs));
          break;
        case SamplerNna::NoteOff:
          v.release();
          break;
        case SamplerNna::Continue:
          break;  // leave it sounding — this is the one that needs no code and all the design
      }
    }

    // 3. ALLOCATE. Free slot first; otherwise steal the QUIETEST, which is the least audible loss
    //    available and is measured from the envelope rather than the last output sample (a voice
    //    at a zero crossing is not a voice that has finished).
    SamplerVoice* target = nullptr;
    for (auto& v : voices_) {
      if (!v.active()) {
        target = &v;
        break;
      }
    }
    bool stolen = false;
    if (!target) {
      float quietest = 1e30f;
      for (auto& v : voices_) {
        if (v.loudness() < quietest) {
          quietest = v.loudness();
          target = &v;
        }
      }
      stolen = true;
      ++steals_;
    }
    if (!target) {
      return;
    }

    SamplerVoiceSpec spec;
    spec.source = audio->view();
    spec.startFrame = slot->startFrame;
    spec.endFrame = slot->endFrame;
    // A SLOT THAT NAMES A SLICE READS THE SLICE'S DERIVED EXTENT, not a stored copy of it. That
    // is what makes a chop re-cuttable while it plays: dragging a marker changes what this slot
    // sounds on its NEXT note, and no note anywhere had to be rewritten. A cached extent would
    // be a second fact about one boundary, and the two would disagree the moment a marker moved.
    if (slot->sliceId != 0) {
      for (const auto& ss : st.sliceSets) {
        if (ss.sourceLocalId != slot->sourceLocalId) {
          continue;
        }
        const SliceExtent ext = sliceExtentById(ss, slot->sliceId, audio->frames);
        if (ext.valid) {
          spec.startFrame = ext.begin;
          spec.endFrame = ext.end;
        } else {
          // The slice was REMOVED. The slot is silent rather than falling back to the whole
          // sample — a chop whose slice is gone should not suddenly play the entire break.
          ++unmapped_;
          return;
        }
        break;
      }
    }
    // THE 9xx SEEK, as a fraction of the slot's extent rather than in absolute frames. Absolute
    // breaks the moment the slot's sample is swapped, and here a slot can name a SLICE, so it
    // breaks on a re-chop too. A fraction survives both.
    if (e.offsetFrac != 0) {
      const uint64_t end = (spec.endFrame == 0 || spec.endFrame > audio->frames) ? audio->frames
                                                                                : spec.endFrame;
      const uint64_t begin = std::min(spec.startFrame, end);
      const uint64_t extent = end - begin;
      spec.startFrame = begin + extent * e.offsetFrac / 65536ull;
    }
    spec.reverse = slot->reverse != 0;
    spec.quality = slot->quality;
    spec.loopStart = slot->loopStartFrame;
    spec.loopEnd = slot->loopEndFrame;
    spec.loopXfade = static_cast<uint32_t>(slot->loopXfadeFrames);
    spec.loopMode = slot->loopMode;
    spec.sustainLoop = slot->sustainLoop;
    spec.gain = db2lin(static_cast<float>(slot->gainMillibels) / 100.0f) *
                (static_cast<float>(e.velocity) / 127.0f);
    spec.pan = static_cast<float>(slot->panThousandths) / 1000.0f;
    spec.ratio = playbackRatio(*slot, e.pitch, audio->sampleRate, snap_->sampleRate);

    const SamplerModSet* mod = st.findModSet(slot->modSetId);
    const SamplerModulator* amp = mod ? mod->ampEnvelope() : nullptr;
    // THE FILTER AND THE OTHER MODULATION TARGETS. The first ENVELOPE aimed at each target wins;
    // a second one aimed at the same target is ignored for now rather than summed, and that is
    // stated here rather than left to be discovered — summing two envelopes is a real feature
    // with real questions (before or after depth? clipped how?) and inventing an answer silently
    // is worse than not having it.
    if (mod) {
      spec.filterType = mod->filterType;
      // cutoffMilli is 0..1000 across the audible range, logarithmically: a linear hertz control
      // spends nine tenths of its travel above 2 kHz where almost nothing musical happens.
      const float norm = static_cast<float>(mod->cutoffMilli) / 1000.0f;
      spec.cutoffHz = 20.0f * std::pow(1000.0f, std::clamp(norm, 0.0f, 1.0f));
      spec.resonance = 0.7f + static_cast<float>(mod->resonanceMilli) / 1000.0f * 9.3f;
      spec.sampleRate = snap_->sampleRate;
      // EVERY ENVELOPE GETS ITS OWN CLOCK, from its OWN modulator. Sharing the amp envelope's
      // was wrong twice over: a mod set with no amp envelope left the clock at zero, so a
      // cutoff sweep never moved at all; and where an amp envelope did exist, a modulator with
      // a different timeBase or rate silently ran at the amp's instead of its own.
      auto unitsPerFrame = [&](const SamplerModulator& m) -> double {
        double u = m.timeBase == 0 ? 1000000.0 / snap_->sampleRate : nanotickPerFrame_;
        if (m.rateMilli != 0) {
          u *= 1000.0 / static_cast<double>(m.rateMilli);
        }
        return u;
      };
      // LFOs. Everything the voice needs is converted HERE — frequency to cycles per frame,
      // depth into the target's own units — so the per-sample path does no unit arithmetic at
      // all and the conversion lives at one boundary, exactly as the envelope clock does.
      //
      // The target scalings are the same as the envelopes': 6 octaves on cutoff, 4800 cents on
      // pitch, the full pan range, the filter's Q range. One modulator's depth means the same
      // thing whichever kind it is.
      auto fillLfo = [&](const SamplerModulator& m, float targetScale,
                         SamplerVoiceSpec::VoiceLfo& out) {
        const float d = static_cast<float>(m.depthMilli) / 1000.0f;
        out.cyclesPerFrame =
            static_cast<float>(m.lfo.frequency_hz / std::max(1.0, snap_->sampleRate));
        out.phase0 = m.lfo.phase_offset;
        out.amp = m.lfo.depth * d * targetScale;
        out.bias = m.lfo.bias * d * targetScale;
        out.active = out.amp != 0.0f || out.bias != 0.0f;
      };
      for (const auto& m : mod->modulators) {
        if (m.kind == ModKind::Lfo) {
          switch (m.target) {
            case ModTarget::Volume:    fillLfo(m, 1.0f, spec.volLfo); break;
            case ModTarget::Panning:   fillLfo(m, 1.0f, spec.panLfo); break;
            case ModTarget::Pitch:     fillLfo(m, 4800.0f, spec.pitchLfo); break;
            case ModTarget::Cutoff:    fillLfo(m, 6.0f, spec.cutoffLfo); break;
            case ModTarget::Resonance: fillLfo(m, 9.3f, spec.resLfo); break;
          }
          continue;
        }
        if (m.kind != ModKind::Envelope || m.env.empty()) {
          continue;
        }
        const float depth = static_cast<float>(m.depthMilli) / 1000.0f;
        switch (m.target) {
          case ModTarget::Cutoff:
            if (!spec.cutoffEnv) {
              spec.cutoffEnv = &m.env;
              spec.cutoffDepth = depth * 6.0f;  // +-6 octaves at full depth
              spec.cutoffUnitsPerFrame = unitsPerFrame(m);
            }
            break;
          case ModTarget::Pitch:
            if (!spec.pitchEnv) {
              spec.pitchEnv = &m.env;
              spec.pitchDepthCents = depth * 4800.0f;  // +-4 octaves
              spec.pitchUnitsPerFrame = unitsPerFrame(m);
            }
            break;
          case ModTarget::Panning:
            if (!spec.panEnv) {
              spec.panEnv = &m.env;
              spec.panDepth = depth;
              spec.panUnitsPerFrame = unitsPerFrame(m);
            }
            break;
          case ModTarget::Resonance:
            if (!spec.resonanceEnv) {
              spec.resonanceEnv = &m.env;
              // The filter's Q runs 0.7..10, so full depth is the whole usable range. It was
              // in the enum and fell through `default` — a target you could name, that did
              // nothing.
              spec.resonanceDepth = depth * 9.3f;
              spec.resonanceUnitsPerFrame = unitsPerFrame(m);
            }
            break;
          default:
            break;
        }
      }
    }
    if (amp) {
      spec.ampEnv = &amp->env;
      // timeBase 0 = microseconds. Nanoticks (1) needs this block's tempo, which the caller
      // supplies via setTempo(); a cached ratio would detune the envelope across a tempo ramp.
      // `rate` scales the clock. Both live in one helper above so the amp envelope and every
      // other envelope cannot disagree about what a unit is.
      double u = amp->timeBase == 0 ? 1000000.0 / snap_->sampleRate : nanotickPerFrame_;
      if (amp->rateMilli != 0) {
        u *= 1000.0 / static_cast<double>(amp->rateMilli);
      }
      spec.envUnitsPerFrame = u;
    }
    // THE VOICE PINS THE SNAPSHOT ITS spec POINTS INTO. Without this the retire list in
    // setSnapshot has nothing to observe — every retired snapshot would look unreferenced and be
    // freed immediately, which is exactly the use-after-free it exists to prevent.
    target->start(spec, e.noteId, slotId, e.column, snap_);
    if (stolen) {
      target->fadeIn(fadeFrames(kStealFadeMs));
    }
  }

  static float db2lin(float db) { return std::pow(10.0f, db / 20.0f); }

  static double playbackRatio(const SamplerSlot& slot,
                              uint8_t pitch,
                              double sourceRate,
                              double engineRate) {
    const double semis = (static_cast<double>(pitch) - static_cast<double>(slot.rootKey)) *
                         (static_cast<double>(slot.pitchTrackMilli) / 1000.0);
    const double cents = static_cast<double>(slot.tuneCents);
    const double r = std::pow(2.0, semis / 12.0 + cents / 1200.0);
    const double rateAdjust = (sourceRate > 0.0 && engineRate > 0.0) ? sourceRate / engineRate : 1.0;
    return r * rateAdjust;
  }

 public:
  // Nanoticks per frame for this block, for tempo-synced envelopes. Set per block by the caller
  // rather than cached, because under a tempo ramp a stale ratio detunes every running envelope.
  void setNanotickPerFrame(double v) { nanotickPerFrame_ = v; }

 private:
  std::vector<SamplerVoice> voices_;
  std::shared_ptr<const SamplerRender> snap_;
  // Snapshots replaced by an edit while voices were still sounding from them. Held until no
  // voice references them; see setSnapshot for why use_count() is a sound test here.
  std::vector<std::shared_ptr<const SamplerRender>> retired_;
  double sampleRate_ = 48000.0;
  double nanotickPerFrame_ = 0.0;
  uint8_t cap_ = 64;
  uint64_t steals_ = 0;
  uint64_t unmapped_ = 0;
  // Per-key round-robin counters, so two toms cycling their alternates do not steal each other's
  // turn. A single global counter would make every round-robin in the kit advance together.
  uint32_t rrCounter_[128]{};
  float* const* stemPlanes_ = nullptr;
  uint32_t stemCount_ = 0;
};

}  // namespace daw
