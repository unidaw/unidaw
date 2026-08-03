#include "engine_track_rebuild.h"

// What the two bodies actually reach for. The file arrived carrying main.cpp's 95 includes,
// which describe where it used to live rather than what it uses.
#include "engine_pure.h"
#include "event_log.h"


namespace daw::engine {

// MOVED FROM main.cpp FILE SCOPE, where it was `inline` and had exactly one caller — the
// function directly below it. The capture enumeration that drives these extractions cannot see
// helpers like this: they are not captured because they are not locals, so they surface only
// when the new translation unit fails to link the name. Third time this session.
//
// It is `static` here rather than `inline` because it now has one home and one user. If a second
// module ever needs it, promote it to a header rather than copying it — a helper written beside
// its only caller is how a function reaches fifteen thousand lines, one reasonable dependency
// at a time.
static std::shared_ptr<const ClipSnapshot> buildClipSnapshot(const daw::MusicalClip& clip) {
  auto snapshot = std::make_shared<ClipSnapshot>();
  snapshot->events = clip.events();
  // The conditional index, in the order the events already are — which is sounding order, so the
  // "previous conditional" a PRE trig asks about is simply the entry before it.
  bool sawPre = false;
  bool sawAnchor = false;
  for (const auto& e : snapshot->events) {
    if (e.type != daw::MusicalEventType::Note ||
        e.payload.note.trigCondition == daw::kTrigConditionNone) {
      continue;
    }
    snapshot->conditionals.push_back(daw::TrigConditionSite{
        e.nanotickOffset, e.payload.note.column, e.payload.note.trigCondition});
    if (daw::isPreTrigCondition(e.payload.note.trigCondition)) {
      sawPre = true;
    } else {
      sawAnchor = true;
    }
  }
  // BY (TICK, COLUMN). The events arrive tick-ordered, and within one tick their order is the
  // flat clip's insertion order — arbitrary to the reader. Sorting by column gives the tie a
  // stated answer (column 0 first) instead of one nobody can predict, and makes the (tick,
  // column) lookup at the dispatch unique.
  std::stable_sort(snapshot->conditionals.begin(), snapshot->conditionals.end(),
                   [](const daw::TrigConditionSite& a, const daw::TrigConditionSite& b) {
                     if (a.tick != b.tick) {
                       return a.tick < b.tick;
                     }
                     return a.column < b.column;
                   });
  // A PRE chain with no A:B anywhere to ground it. Recorded for the caller to report ONCE.
  snapshot->unanchoredPre = sawPre && !sawAnchor;
  return snapshot;
}


std::shared_ptr<const ClipSnapshot> rebuildFlatAndPublish(FlatRebuildDeps& deps,
                                                          TrackRuntime& rt) {
  auto& laneQuantizeOf = deps.laneQuantizeOf;
  auto& trackWindowEnd = deps.trackWindowEnd;

    // A MUTE OUTLIVING ITS BASE NOTE keeps the override badge lit over nothing. Mute a note on
    // one appearance, then delete that note from the CLIP, and the mute record survives pointing
    // at a note id that no longer exists: the extent still publishes an override count and the
    // local-edits flag, so the rail says "this appearance is customised" and there is nothing to
    // find. Reverting the overrides then "clears" something inaudible.
    //
    // Pruned HERE because this is the single funnel every structural change goes through, so a
    // dead mute cannot survive past one rebuild — and it also cleans up mutes orphaned by a clip
    // swap or by loading a file whose ids do not line up, which keying on the removed id would
    // miss. Only when the referenced clip EXISTS: if it is absent we cannot tell a dead mute
    // from one whose clip has not been installed yet, and guessing would delete real overrides.
    uint32_t prunedMutes = 0;
    for (auto& pl : rt.sourcePlacements) {
      if (pl.mutes.empty()) {
        continue;
      }
      const daw::ProjectClip* clipDef = nullptr;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clipDef = &c;
          break;
        }
      }
      if (!clipDef) {
        continue;
      }
      const size_t before = pl.mutes.size();
      pl.mutes.erase(
          std::remove_if(pl.mutes.begin(), pl.mutes.end(),
                         [&](daw::EventId id) {
                           for (const auto& e : clipDef->clip.events()) {
                             if (e.type == daw::MusicalEventType::Note &&
                                 e.payload.note.noteId == id) {
                               return false;
                             }
                           }
                           return true;
                         }),
          pl.mutes.end());
      prunedMutes += static_cast<uint32_t>(before - pl.mutes.size());
    }
    if (prunedMutes > 0) {
      DAW_EVENT("local_edit.mutes_pruned")
          .field("track", rt.trackId)
          .field("count", prunedMutes)
          .field("reason", "base_note_gone");
    }
    const uint64_t windowEnd = trackWindowEnd(rt);
    daw::MusicalClip flat;
    for (const auto& ev :
         daw::flattenPlacements(rt.sourcePlacements, rt.ownedClips, windowEnd)) {
      flat.addEvent(ev);
    }
    rt.track.clip = std::move(flat);
    // THE WIDEST OP RUN, over the flat clip that was just built. Counted as GLYPHS — one per op
    // present — because that is what the collapsed cell draws; see ShmHeader::uiTrackOpsWidth.
    //
    // Here rather than in the publish loop: this is the single funnel every structural change
    // goes through, so the number moves with clipVersion, and a max over every note in the track
    // is not something to do once per block.
    {
      uint8_t widest = 0;
      for (const auto& ev : rt.track.clip.events()) {
        if (ev.type != daw::MusicalEventType::Note) {
          continue;
        }
        const auto& n = ev.payload.note;
        const uint8_t count =
            static_cast<uint8_t>((n.retrigger > 1 ? 1 : 0) + (n.probability > 0 ? 1 : 0) +
                                 (n.delayNanoticks > 0 ? 1 : 0) + (n.sound != 0 ? 1 : 0) +
                                 (n.soundOffset != 0 ? 1 : 0) + (n.retrigRamp != 0 ? 1 : 0) +
                                 (n.trigCondition != 0 ? 1 : 0));
        widest = std::max(widest, count);
      }
      rt.opsWidth.store(widest, std::memory_order_relaxed);
    }
    rt.clipExtents.clear();
    for (size_t i = 0; i < rt.sourcePlacements.size(); ++i) {
      const auto& pl = rt.sourcePlacements[i];
      if (!pl.at.has_value()) {
        continue;
      }
      ClipExtentInfo ext;
      ext.placementId = pl.id;  // stable placement id (was the list index — now survives edits)
      ext.clipId = pl.clipId;
      ext.at = *pl.at;
      // THE SAME THREE-STEP RULE locateEditTarget USES, and it did not before: an explicit
      // placement length, else the clip's own loop length, else — for a LINEAR length-0 clip,
      // which plays once and does not loop — the clip's CONTENT end.
      //
      // Missing the third step published startTick == endTick for such a placement, so a client
      // testing containment found it EMPTY. The web UI's shared-clip warning went silent on
      // exactly the placement somebody had just created, which is when they are most likely to
      // type into it. Two answers to "how far does this placement reach": note entry said it
      // covers its content, the published extent said it covers nothing.
      // THIS SITE IS WHERE THE THREE-LEVEL FALLBACK CAME FROM — the other four had only two, and
      // measured a zero-length clip's placement as empty. The loop stays because it also reads
      // the clip's NAME and KIND, which are not length; only the length rule moved out.
      const uint64_t length = daw::engine::placementLength(pl, rt.ownedClips);
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          ext.name = c.name;
          ext.isAudio = c.kind == daw::ClipKind::Audio;
          break;
        }
      }
      ext.endTick = *pl.at + length;
      ext.overrideCount =
          static_cast<uint32_t>(pl.adds.size() + pl.mutes.size());
      ext.localEdits = pl.localEdits;
      ext.hasAlternate = pl.alternateClipId != 0;
      rt.clipExtents.push_back(std::move(ext));
    }
    // M1.13: the clip the UI draws and the clip that SOUNDS are already two objects —
    // rt.track.clip is published and saved, the returned snapshot is what the producer
    // schedules from. Quantize applies to the second only. Doing it here rather than at
    // emission time matters: moving a note's start changes which block it belongs to,
    // and the scheduler windows on the tick it reads, so a note nudged earlier at
    // emission time would already have missed its block.
    auto built = buildClipSnapshot(
        daw::quantizeClipForSchedule(rt.track.clip, laneQuantizeOf(rt)));
    // A PRE trig with no A:B anywhere on the track to ground it. Said out loud, once per
    // rebuild, because the alternative is a row whose behaviour nobody can account for: a lone
    // one is silent forever and a run of them unwinds to the recursion cap and sounds. Neither
    // is what was typed, and neither leaves a trace anywhere else.
    if (built && built->unanchoredPre) {
      DAW_EVENT("trig.unanchored_pre")
          .field("track", rt.trackId)
          .field("conditionals", static_cast<uint64_t>(built->conditionals.size()))
          .field("detail",
                 "a PRE trig has no A:B conditional on this track to resolve against");
    }
    return built;
}

std::shared_ptr<const AudioRenderList> rebuildAudioRender(AudioRenderRebuildDeps& deps,
                                                          const TrackRuntime& rt) {
  auto& engineConfig = deps.engineConfig;
  auto& internDecodedForWaveform = deps.internDecodedForWaveform;
  auto& resolveSourcePath = deps.resolveSourcePath;
  auto& tickConverter = deps.tickConverter;
  auto& waveformStore = deps.waveformStore;

    auto list = std::make_shared<AudioRenderList>();
    const double rate = static_cast<double>(engineConfig.sampleRate);
    auto toSamples = [&](uint64_t ticks) -> int64_t {
      return tickConverter.nanoticksToSamplesAbsolute(ticks);
    };
    // Small per-rebuild decode cache so one file placed twice decodes once.
    struct Cached {
      std::string path;  // resolved absolute
      std::shared_ptr<const AudioSourceBuffer> samples;
      uint64_t frames;
      double srcRate;
    };
    std::vector<Cached> cache;
    for (const auto& pl : rt.sourcePlacements) {
      if (!pl.at.has_value()) {
        continue;
      }
      const daw::ProjectClip* clip = nullptr;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clip = &c;
          break;
        }
      }
      if (!clip || clip->kind != daw::ClipKind::Audio ||
          clip->audio.sourcePath.empty()) {
        continue;
      }
      // Resolve the source relative to the project (portable, not CWD-bound).
      const std::string resolvedPath = resolveSourcePath(clip->audio.sourcePath);
      std::shared_ptr<const AudioSourceBuffer> src;
      uint64_t frames = 0;
      double srcRate = rate;
      bool have = false;
      for (const auto& c : cache) {
        if (c.path == resolvedPath) {
          src = c.samples;
          frames = c.frames;
          srcRate = c.srcRate;
          have = true;
          break;
        }
      }
      if (!have) {
        auto dec = daw::decodeAudioFile(resolvedPath);
        if (!dec.ok) {
          // Surface the miss: a silent `continue` is how a missing sample file
          // becomes "the waveform feature is broken" three weeks later. The failed
          // descriptor lets the UI draw the path instead of an empty box.
          waveformStore.internFailed(resolvedPath);
          DAW_EVENT("audio.decode_failed")
              .field("clip", clip->id)
              .field("path", resolvedPath);
          continue;
        }
        {
          auto buf = std::make_shared<AudioSourceBuffer>();
          buf->channels = std::move(dec.channels);
          buf->frames = dec.frames;
          buf->buildPlanes();
          src = std::move(buf);
        }
        frames = dec.frames;
        srcRate = dec.sampleRate;
        cache.push_back({resolvedPath, src, frames, srcRate});

        // Register the pyramid for waveform display, keyed on a content hash of the
        // file's identity + decoded shape (contract §5), so two placements share one
        // entry and a re-bounce in place invalidates it. Shared with the sampler's
        // decode — see internDecodedForWaveform for why that is one definition.
        const auto& py = dec.pyramid;
        const uint32_t sourceId = internDecodedForWaveform(resolvedPath, dec);
        DAW_EVENT("audio.source_ready")
            .field("sourceId", sourceId)
            .field("frames", dec.frames)
            .field("channels", dec.sourceChannels)
            .field("absPeak", py ? py->absPeak : 0.0f)
            .field("levelMask", py ? py->levelMask : 0u)
            .field("path", resolvedPath);
      }
      // BEHAVIOUR IS UNCHANGED HERE, and that is worth saying rather than leaving to be
      // rediscovered: this path is already guarded to Audio clips, and an audio clip carries no
      // MusicalClip, so the third fallback level yields 0 exactly as the old two-level ternary
      // did. It uses the shared rule anyway — a fifth private copy of a rule with two known
      // divergences is how the sixth gets written.
      const uint64_t lenTicks = daw::engine::placementLength(pl, rt.ownedClips);
      AudioRegionRender r;
      r.params.regionStartSample = toSamples(*pl.at);
      r.params.regionLengthSamples = toSamples(lenTicks);
      r.params.sourceStartFrame = clip->audio.sourceStartFrame;
      r.params.sourceRate = srcRate;
      r.params.engineRate = rate;
      r.params.gain = static_cast<float>(std::pow(10.0, clip->audio.gainDb / 20.0));
      r.params.fadeInSamples = toSamples(clip->audio.fadeInNanoticks);
      r.params.fadeOutSamples = toSamples(clip->audio.fadeOutNanoticks);
      r.source = std::move(src);
      r.sourceFrames = frames;
      // What the engine actually scheduled, in samples. Absolute positioning is
      // invisible from the outside until it is wrong by a whole bar, and then it reads
      // as "the audio is in the wrong place" with nothing to compare against.
      DAW_EVENT("audio.region_scheduled")
          .field("track", rt.trackId)
          .field("clip", clip->id)
          .field("at_tick", *pl.at)
          .field("start_sample", static_cast<uint64_t>(r.params.regionStartSample))
          .field("length_samples", static_cast<uint64_t>(r.params.regionLengthSamples));
      list->push_back(std::move(r));
    }
    return list;
}

}  // namespace daw::engine
