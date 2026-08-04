// Bodies for apps/engine_clip_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_clip_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"

namespace daw::engine {

void handleSetClipGrid(ClipCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  const auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  daw::UiSetClipGridPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  if (p.flags == 0) {
    DAW_EVENT("clip.grid_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("reason", "no_field_named");
    return;
  }
  // REFUSED, NOT CLAMPED, and each bound is the packer's rather than an opinion: five bits
  // for the subdivision and the numerator, and a 3-bit EXPONENT for the denominator, so it
  // must be a power of two. Clamping is packClipGrid's defence against a bad value reaching
  // the wire; at this layer an out-of-range value is a caller with the wrong idea of the
  // unit, and rounding a non-power-of-two denominator hands back a meter nobody asked for.
  const char* bad = nullptr;
  if ((p.flags & daw::kClipGridSetLines) != 0 &&
      (p.linesPerBeat == 0 || p.linesPerBeat > 31)) {
    bad = "lines_out_of_range";
  } else if ((p.flags & daw::kClipGridSetNumerator) != 0 &&
             (p.timeSigNumerator == 0 || p.timeSigNumerator > 31)) {
    bad = "numerator_out_of_range";
  } else if ((p.flags & daw::kClipGridSetDenominator) != 0 &&
             (p.timeSigDenominator == 0 || p.timeSigDenominator > 128 ||
              (p.timeSigDenominator & (p.timeSigDenominator - 1)) != 0)) {
    bad = "denominator_not_power_of_two";
  }
  if (bad != nullptr) {
    DAW_EVENT("clip.grid_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("lines", p.linesPerBeat)
        .field("num", p.timeSigNumerator)
        .field("den", p.timeSigDenominator)
        .field("reason", bad);
    return;
  }
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, p.trackId);
  if (!runtime) {
    DAW_EVENT("clip.grid_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("reason", "no_such_track");
    return;
  }
  bool applied = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& oc : runtime->ownedClips) {
      if (oc.id != p.clipId) {
        continue;
      }
      // The flags are what makes "change the meter, keep the subdivision" expressible. There
      // is no value that could mean "leave this alone": 0 is the packer's sentinel for no
      // grid, not a spare.
      if ((p.flags & daw::kClipGridSetLines) != 0) {
        oc.linesPerBeat = p.linesPerBeat;
      }
      if ((p.flags & daw::kClipGridSetNumerator) != 0) {
        oc.timeSigNumerator = p.timeSigNumerator;
      }
      if ((p.flags & daw::kClipGridSetDenominator) != 0) {
        oc.timeSigDenominator = p.timeSigDenominator;
      }
      applied = true;
      break;
    }
  }
  if (!applied) {
    DAW_EVENT("clip.grid_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("reason", "no_such_clip");
    return;
  }
  // BUMP THE CLIP VERSION, or the edit saves and is never SEEN. The extent publisher does
  // read each clip's grid live out of ownedClips — but the whole publish is gated on
  // `clipVersion` moving (writeUiClipExtents returns early when it has not), so a change that
  // touches no note republishes nothing. Measured before this line existed: the command
  // applied, the save carried 3 and 7/8, and `get extents` still said 4 and 4/4 — the exact
  // "written and not drawn" half of the defect this command exists to fix.
  //
  // Through the helper rather than by hand: it advances the per-track counter and the global
  // gate in that order, and the comment on it explains why the reverse deadlocks a track's
  // published base one version behind forever.
  bumpClipVersionFor(runtime);
  DAW_EVENT("clip.grid_set")
      .field("track", p.trackId)
      .field("clip", p.clipId)
      .field("lines", p.linesPerBeat)
      .field("num", p.timeSigNumerator)
      .field("den", p.timeSigDenominator);
  return;
}

void handleSetAudioClipField(ClipCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  const auto& publishAudioClipTable = deps.publishAudioClipTable;
  const auto& rebuildAudioRender = deps.rebuildAudioRender;
  daw::UiAudioClipFieldPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  const auto field = static_cast<daw::AudioClipField>(p.field);
  const char* fieldName = nullptr;
  switch (field) {
    case daw::AudioClipField::SourceStartFrame: fieldName = "source_start_frame"; break;
    case daw::AudioClipField::GainMillibels: fieldName = "gain_millibels"; break;
    case daw::AudioClipField::FadeInNanoticks: fieldName = "fade_in_nanoticks"; break;
    case daw::AudioClipField::FadeOutNanoticks: fieldName = "fade_out_nanoticks"; break;
  }
  if (fieldName == nullptr) {
    DAW_EVENT("audio_clip.field_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("field", static_cast<uint64_t>(p.field))
        .field("reason", "no_field_named");
    return;
  }
  // THE THREE TIME/FRAME FIELDS REFUSE A NEGATIVE. Negative is not "before the start", it is
  // a caller with the wrong idea of the unit, and there is no natural limit to clamp toward —
  // so refusing says so where clamping to 0 would silently accept a bug.
  if (field != daw::AudioClipField::GainMillibels && p.value < 0) {
    DAW_EVENT("audio_clip.field_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("field", fieldName)
        .field("value", p.value)
        .field("reason", "negative_not_allowed");
    return;
  }
  // GAIN IS CLAMPED, and that is not this file's habit relaxed. It is what the sampler slot
  // does with exactly this quantity over exactly this range, because a gain is a continuous
  // control with natural limits and a fader stopping at the end of its travel is what a
  // person expects from a drag. Inventing a second policy for the same quantity one object
  // along would be worse than either policy applied consistently.
  constexpr int64_t kMinGainMillibels = -9600;
  constexpr int64_t kMaxGainMillibels = 2400;
  int64_t value = p.value;
  bool clamped = false;
  if (field == daw::AudioClipField::GainMillibels) {
    const int64_t before = value;
    value = std::max(kMinGainMillibels, std::min(kMaxGainMillibels, value));
    clamped = value != before;
  }

  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, p.trackId);
  if (!runtime) {
    DAW_EVENT("audio_clip.field_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("field", fieldName)
        .field("reason", "no_such_track");
    return;
  }
  bool applied = false;
  bool wrongKind = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& oc : runtime->ownedClips) {
      if (oc.id != p.clipId) {
        continue;
      }
      // A SYMBOLIC CLIP HAS NO GAIN. Writing one would set a field the save path never emits
      // for this kind and the renderer never reads — accepted, invisible, and gone on
      // reload. Named rather than ignored, because "the command succeeded and nothing
      // happened" is the failure this whole opcode exists to remove.
      if (oc.kind != daw::ClipKind::Audio) {
        wrongKind = true;
        break;
      }
      switch (field) {
        case daw::AudioClipField::SourceStartFrame:
          oc.audio.sourceStartFrame = static_cast<uint64_t>(value);
          break;
        case daw::AudioClipField::GainMillibels:
          oc.audio.gainDb = static_cast<double>(value) / 100.0;
          break;
        case daw::AudioClipField::FadeInNanoticks:
          oc.audio.fadeInNanoticks = static_cast<uint64_t>(value);
          break;
        case daw::AudioClipField::FadeOutNanoticks:
          oc.audio.fadeOutNanoticks = static_cast<uint64_t>(value);
          break;
      }
      applied = true;
      break;
    }
  }
  if (wrongKind) {
    DAW_EVENT("audio_clip.field_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("field", fieldName)
        .field("reason", "not_an_audio_clip");
    return;
  }
  if (!applied) {
    DAW_EVENT("audio_clip.field_rejected")
        .field("track", p.trackId)
        .field("clip", p.clipId)
        .field("field", fieldName)
        .field("reason", "no_such_clip");
    return;
  }
  // RE-DERIVE THE RENDER, or the edit is saved and never HEARD. rebuildAudioRender bakes the
  // gain into a linear multiplier and the fades and in-point into sample counts at build
  // time; the audio thread reads only that baked list. Without this line the model changes,
  // the file is right, the table below reports the new number, and the clip plays at the old
  // gain until some unrelated edit happens to rebuild — the audio-domain twin of opcode 94's
  // "saved correctly and drew nothing".
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                               std::memory_order_release);
  }
  // AND REPUBLISH THE TABLE, or the edit is never SEEN. Same argument one layer out: the
  // descriptor table carried these four fields as a load-time snapshot.
  publishAudioClipTable();
  DAW_EVENT("audio_clip.field_set")
      .field("track", p.trackId)
      .field("clip", p.clipId)
      .field("field", fieldName)
      .field("value", value)
      .field("clamped", clamped ? 1u : 0u);
  return;
}

}  // namespace daw::engine
