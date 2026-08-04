#include "engine_publish_clips.h"

// What the two bodies reach for beyond the module header. This file arrived carrying
// main.cpp's 99 includes, which described where it used to live rather than what it uses.
#include "clip_grid.h"
#include "event_log.h"


namespace daw::engine {

void writeUiClipExtents(ClipExtentsDeps& deps, bool force) {
  auto& clipVersion = deps.clipVersion;
  auto& lastClipExtentVersion = deps.publishGates.lastClipExtentVersion;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& uiShm = deps.uiShm;

    if (!uiShm.header || uiShm.header->uiClipExtentOffset == 0) {
      return;
    }
    const uint32_t clipVersionValue = clipVersion.load(std::memory_order_acquire);
    if (!force && clipVersionValue == lastClipExtentVersion) {
      return;
    }
    lastClipExtentVersion = clipVersionValue;
    const auto freshTracks = snapshotTracks();
    auto* region = reinterpret_cast<daw::UiClipExtentRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiClipExtentOffset);
    uint32_t count = 0;
    uint32_t extentsDropped = 0;
    for (auto* runtime : freshTracks) {
      if (!runtime) {
        continue;
      }
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // Always publish the authored clip extents. rebuildFlatAndPublish rebuilds
      // rt.clipExtents from the structural store (placements + owned clips) on every
      // edit, so they describe the notes even on a live-edited track — carrying the
      // real clipId (which joins UiAudioClip and the per-clip grid) that the old
      // arrangement-dirty segmentation path zeroed. That path predated the structural
      // store; segmenting the flat clip dropped clip identity, made audio rails vanish
      // on a note edit, and published no grid. See the frontend's P1.
      for (const auto& ext : runtime->clipExtents) {
        if (count >= daw::kUiMaxClipExtents) {
          // COUNT the shortfall rather than walking away from it. Keep going so the number is
          // the real total that did not fit, not just "at least one".
          ++extentsDropped;
          continue;
        }
        daw::UiClipExtent& out = region->extents[count];
        out.placementId = ext.placementId;
        out.clipId = ext.clipId;
        out.trackId = runtime->trackId;
        uint32_t extFlags = ext.isAudio ? daw::kUiClipExtentAudio : 0u;
        // M3.24: the override badge — how far THIS APPEARANCE differs from its clip.
        // Saturating at 255 with a separate has-overrides bit, so a big count can never
        // read as none.
        extFlags |= daw::packClipExtentOverrides(ext.overrideCount);
        if (ext.localEdits) {
          extFlags |= daw::kUiClipExtentLocalEdits;
        }
        if (ext.hasAlternate) {
          extFlags |= daw::kUiClipExtentHasAlternate;
        }
        // Pack the clip's own musical grid into the spare flag bits (0 => the reader
        // falls back to the song meter). Clamp + refuse loudly per the three rules.
        for (const auto& oc : runtime->ownedClips) {
          if (oc.id != ext.clipId) {
            continue;
          }
          const auto g = daw::packClipGrid(oc.linesPerBeat, oc.timeSigNumerator,
                                           oc.timeSigDenominator);
          extFlags |= g.bits;
          if (g.outcome == daw::ClipGridOutcome::ClampedLpb ||
              g.outcome == daw::ClipGridOutcome::ClampedNum) {
            DAW_EVENT("project.meter_clamped")
                .field("clip", ext.clipId)
                .field("field",
                       std::string(g.outcome == daw::ClipGridOutcome::ClampedLpb
                                       ? "lines_per_beat"
                                       : "time_sig_numerator"))
                .field("asked", g.asked)
                .field("stored", g.stored);
          } else if (g.outcome == daw::ClipGridOutcome::RefusedDen) {
            DAW_EVENT("project.meter_refused")
                .field("clip", ext.clipId)
                .field("field", std::string("time_sig_denominator"))
                .field("asked", g.asked);
          }
          break;
        }
        out.flags = extFlags;
        out.startTick = ext.at;
        out.endTick = ext.endTick;
        std::memset(out.name, 0, sizeof(out.name));
        std::memcpy(out.name, ext.name.data(),
                    std::min(ext.name.size(), sizeof(out.name) - 1));
        ++count;
      }
    }
    region->count = count;
    region->truncated = extentsDropped;
    if (extentsDropped > 0) {
      // Said out loud as well as published: the region's own reader may be a UI that draws what
      // it is given without checking, and the person needs to know the rails are incomplete.
      DAW_EVENT("clip_extents.truncated")
          .field("published", count)
          .field("dropped", extentsDropped)
          .field("cap", static_cast<uint64_t>(daw::kUiMaxClipExtents));
    }
}

void publishAudioClipTable(AudioClipTableDeps& deps) {
  auto& loadedClips = deps.loadedProject.loadedClips;
  auto& loadedClipsMutex = deps.loadedProject.loadedClipsMutex;
  auto& resolveSourcePath = deps.resolveSourcePath;
  auto& tempoProvider = deps.tempoProvider;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& uiShm = deps.uiShm;
  auto& waveformStore = deps.waveformStore;

    if (!uiShm.header || uiShm.header->uiAudioSourceOffset == 0) {
      return;
    }
    auto* region = reinterpret_cast<daw::UiAudioSourceRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAudioSourceOffset);

    // THE SOURCE HALF, AND IT LIVES HERE FOR THE REASON THE CLIP HALF DOES. This loop was
    // load-only until SetClipText (98) let a command repoint a clip at another file: the retarget
    // interned the new source and moved the clip's sourceId to it, and the sources array still
    // listed only what the load had seen. The published table then carried a clip pointing at a
    // sourceId that was not in it — a DANGLING JOIN, which is worse than the stale path it
    // replaced, because a reader gets no waveform, no path, and nothing saying why.
    //
    // The comment above the old call site already said a load-only copy of the clip loop "is
    // exactly how the table came to be a load-time snapshot in the first place". It was right,
    // and the source loop sitting immediately above it was still that copy. Both halves are one
    // definition now, so the next writer cannot update one and leave the other behind.
    const auto sources = waveformStore.snapshot();
    uint32_t sourceCount = 0;
    for (const auto& e : sources) {
      if (sourceCount >= daw::kUiMaxAudioSources) break;
      auto& d = region->sources[sourceCount++];
      d = daw::UiAudioSource{};
      d.sourceId = e.sourceId;
      d.contentKeyLo = static_cast<uint32_t>(e.contentKey & 0xffffffffu);
      d.contentKeyHi = static_cast<uint32_t>(e.contentKey >> 32);
      d.sourceChannels = e.sourceChannels;
      d.waveChannels = e.waveChannels;
      d.status = e.status;
      d.sourceFrames = e.sourceFrames;
      d.sourceRateHz = e.sourceRateHz;
      d.absPeak = e.absPeak;
      d.levelMask = e.levelMask;
      std::memcpy(d.path, e.path.c_str(),
                  std::min(e.path.size(), sizeof(d.path) - 1));
      d.flags = (e.channelsTruncated ? 1u : 0u) | (e.clipped ? 2u : 0u);
    }
    for (uint32_t i = sourceCount; i < daw::kUiMaxAudioSources; ++i) {
      region->sources[i] = daw::UiAudioSource{};
    }
    region->sourceCount = sourceCount;
    region->formatVersion = daw::kWaveformFormatVersion;
    // The constant tempo audio is actually positioned at (bpmAtNanotick(0)) — the number
    // rebuildAudioRender uses, so drawn == heard even on a tempo-mapped project where audio is
    // not yet tempo-followed. See contract §2.4.
    region->audioMapBpmMilli =
        static_cast<uint32_t>(tempoProvider.bpmAtNanotick(0) * 1000.0 + 0.5);

    std::vector<TrackRuntime*> runtimes;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      runtimes.reserve(tracks.size());
      for (const auto& t : tracks) {
        if (t) {
          runtimes.push_back(t.get());
        }
      }
    }

    uint32_t clipCount = 0;
    uint32_t audioClipsDropped = 0;
    std::vector<uint32_t> published;
    auto emit = [&](const daw::ProjectClip& c) {
      if (c.kind != daw::ClipKind::Audio || c.audio.sourcePath.empty()) {
        return;
      }
      if (std::find(published.begin(), published.end(), c.id) != published.end()) {
        return;
      }
      published.push_back(c.id);
      // kUiMaxAudioClips is 64 while the extent list holds 256, so this cap can be reached
      // while the rails look complete — a box with no waveform in it and nothing saying why.
      // Count the shortfall and keep going so the number is the real total.
      if (clipCount >= daw::kUiMaxAudioClips) {
        ++audioClipsDropped;
        return;
      }
      auto& d = region->clips[clipCount++];
      d = daw::UiAudioClip{};
      d.clipId = c.id;
      d.sourceId = waveformStore.sourceIdForPath(resolveSourcePath(c.audio.sourcePath));
      d.sourceStartFrame = c.audio.sourceStartFrame;
      d.clipLengthTicks = c.lengthNanoticks;
      d.fadeInTicks = static_cast<uint32_t>(c.audio.fadeInNanoticks);
      d.fadeOutTicks = static_cast<uint32_t>(c.audio.fadeOutNanoticks);
      d.gainDb = c.audio.gainDb;
    };

    for (TrackRuntime* rt : runtimes) {
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      for (const auto& c : rt->ownedClips) {
        emit(c);
      }
    }
    {
      std::lock_guard<std::mutex> lock(loadedClipsMutex);
      for (const auto& c : loadedClips) {
        emit(c);
      }
    }

    for (uint32_t i = clipCount; i < daw::kUiMaxAudioClips; ++i) {
      region->clips[i] = daw::UiAudioClip{};
    }
    region->clipCount = clipCount;
    region->clipsTruncated = audioClipsDropped;
    if (audioClipsDropped > 0) {
      DAW_EVENT("audio_clips.truncated")
          .field("published", clipCount)
          .field("dropped", audioClipsDropped)
          .field("cap", static_cast<uint64_t>(daw::kUiMaxAudioClips));
    }
    // Version last, behind a release fence, so a reader seeing the new version sees the
    // complete table — the same discipline deviceParams uses.
    std::atomic_thread_fence(std::memory_order_release);
    region->version += 1;
}

}  // namespace daw::engine
