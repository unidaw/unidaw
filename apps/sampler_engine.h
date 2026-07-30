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

  void setSnapshot(std::shared_ptr<const SamplerRender> snap) { snap_ = std::move(snap); }
  const SamplerRender* snapshot() const { return snap_.get(); }

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

  // Renders one block. `events` must be sorted by offsetInBlock; the caller has them in order
  // already because they come off a time-ordered schedule.
  void render(float* const* out,
              uint32_t outChannels,
              uint32_t numFrames,
              const SamplerEvent* events,
              uint32_t numEvents) {
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
          if (v.active()) {
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
    if (amp) {
      spec.ampEnv = &amp->env;
      // timeBase 0 = microseconds. Nanoticks (1) needs this block's tempo, which the caller
      // supplies via setTempo(); a cached ratio would detune the envelope across a tempo ramp.
      spec.envUnitsPerFrame =
          amp->timeBase == 0 ? 1000000.0 / snap_->sampleRate : nanotickPerFrame_;
      // `rate` scales the envelope's clock. Applied here rather than inside the runner so the
      // runner stays a pure function of frames and its unit conversion stays at one boundary.
      if (amp->rateMilli != 0) {
        spec.envUnitsPerFrame *= 1000.0 / static_cast<double>(amp->rateMilli);
      }
    }
    target->start(spec, e.noteId, slotId, e.column);
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
  double sampleRate_ = 48000.0;
  double nanotickPerFrame_ = 0.0;
  uint8_t cap_ = 64;
  uint64_t steals_ = 0;
  uint64_t unmapped_ = 0;
  // Per-key round-robin counters, so two toms cycling their alternates do not steal each other's
  // turn. A single global counter would make every round-robin in the kit advance together.
  uint32_t rrCounter_[128]{};
};

}  // namespace daw
