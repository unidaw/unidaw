// Bodies for apps/engine_sampler_commands.h. Each moved verbatim out of handleUiEntry;
// see the header for why they are void and what that preserves.
#include "apps/engine_sampler_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

#include "apps/event_log.h"
#include "apps/sampler_slice.h"
#include "apps/sampler_serialize.h"
#include "apps/musical_structures.h"

namespace daw::engine {

void handleSamplerEmitRows(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerEmitRowsPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerEmitRows, daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.emit_rejected").field("track", p.trackId).field("reason", "no_such_track");
    return;
  }

  // Collect (sliceId -> the slot that plays it, and that slot's key) from the SNAPSHOT, so
  // the rows written match what the engine will actually sound.
  struct Row {
    uint16_t sliceId = 0;
    uint16_t slotId = 0;
    uint8_t key = 60;
    uint64_t frame = 0;
  };
  std::vector<Row> rows;
  double sampleRate = 48000.0;
  uint64_t sourceFrames = 0;
  {
    std::shared_ptr<const daw::SamplerRender> snap;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snap = runtime->samplerSnapshot;
    }
    if (!snap) {
      reportSamplerReject(daw::UiCommandType::SamplerEmitRows, daw::UiSamplerRejectReason::NotASampler, p.trackId, 0, 0);
      DAW_EVENT("sampler.emit_rejected").field("track", p.trackId).field("reason", "no_sampler");
      return;
    }
    const daw::SamplerSourceAudio* audio =
        snap->audioFor(static_cast<uint16_t>(p.sourceLocalId));
    if (!audio) {
      reportSamplerReject(daw::UiCommandType::SamplerEmitRows, daw::UiSamplerRejectReason::NoSuchSource, p.trackId, 0,
                          static_cast<uint16_t>(p.sourceLocalId));
      DAW_EVENT("sampler.emit_rejected")
          .field("track", p.trackId)
          .field("source", p.sourceLocalId)
          .field("reason", "no_such_source");
      return;
    }
    sampleRate = audio->sampleRate > 0 ? audio->sampleRate : 48000.0;
    sourceFrames = audio->frames;
    for (const auto& ss : snap->state.sliceSets) {
      if (ss.sourceLocalId != static_cast<uint16_t>(p.sourceLocalId)) {
        continue;
      }
      for (const auto& m : ss.markers) {
        Row r;
        r.sliceId = m.id;
        r.frame = m.frame;
        for (const auto& sl : snap->state.slots) {
          if (sl.sliceId == m.id) {
            r.slotId = sl.id;
            r.key = sl.rootKey;
          }
        }
        // A slice with NO SLOT is skipped rather than emitted with sound 0 — sound 0 means
        // "let pitch pick", which would silently play whatever the keymap says instead of
        // that slice. A row that plays the wrong audio is worse than a row that is absent.
        if (r.slotId != 0) {
          rows.push_back(r);
        }
      }
    }
  }
  if (rows.empty()) {
    reportSamplerReject(daw::UiCommandType::SamplerEmitRows, daw::UiSamplerRejectReason::NoSuchSliceSet, p.trackId, 0,
                        static_cast<uint16_t>(p.sourceLocalId));
    DAW_EVENT("sampler.emit_rejected")
        .field("track", p.trackId)
        .field("source", p.sourceLocalId)
        .field("reason", "no_sliced_slots");
    return;
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.frame < b.frame; });

  // THE ROWS ARE THE TIMING. With time-stretch rejected, this is HOW a 174 bpm break plays
  // at 140: each slice starts on its own row, and the rows follow the project tempo. Nothing
  // is stretched and nothing has to be.
  //
  // A step of 0 means "derive it": space the rows by each slice's own LENGTH, converted to
  // ticks at the current tempo, which reproduces the break at the tempo it was recorded at.
  // An explicit step re-fits it to a grid instead.
  const double bpm = tempoProvider.bpmAtNanotick(p.atNanotick);
  const double ticksPerFrame =
      (60.0 * static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter)) /
      ((bpm > 0.0 ? bpm : 120.0) * sampleRate);
  uint32_t written = 0;
  uint64_t tick = p.atNanotick;
  for (size_t i = 0; i < rows.size(); ++i) {
    const uint64_t nextFrame =
        (i + 1 < rows.size()) ? rows[i + 1].frame : sourceFrames;
    const uint64_t sliceFrames = nextFrame > rows[i].frame ? nextFrame - rows[i].frame : 0;
    const uint64_t step =
        p.stepNanoticks > 0
            ? p.stepNanoticks
            : static_cast<uint64_t>(static_cast<double>(sliceFrames) * ticksPerFrame);
    if (step == 0) {
      continue;
    }
    const uint16_t flags = static_cast<uint16_t>(p.column);
    if (applyAddNote(p.trackId, tick, step, rows[i].key, p.velocity, flags,
                     /*recordUndo=*/written == 0, std::nullopt, rows[i].slotId, 0)) {
      ++written;
    }
    tick += step;
  }
  DAW_EVENT("sampler.rows_emitted")
      .field("track", p.trackId)
      .field("source", p.sourceLocalId)
      .field("rows", written)
      .field("at", p.atNanotick)
      .field("end", tick);
  return;
}

void handleSamplerSlice(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  const bool isSlice = commandType == daw::UiCommandType::SamplerSlice;
  daw::UiSamplerSlicePayload sp{};
  daw::UiSamplerMarkerPayload mp{};
  if (isSlice) {
    std::memcpy(&sp, entry.payload, sizeof(sp));
  } else {
    std::memcpy(&mp, entry.payload, sizeof(mp));
  }
  const uint32_t trackId = isSlice ? sp.trackId : mp.trackId;
  const uint32_t deviceId = isSlice ? sp.deviceId : mp.deviceId;
  const uint32_t sourceId = isSlice ? sp.sourceLocalId : mp.sourceLocalId;

  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (trackId < tracks.size()) {
      runtime = tracks[trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSlice, daw::UiSamplerRejectReason::NoSuchTrack, trackId, 0, 0);
    DAW_EVENT("sampler.slice_rejected").field("track", trackId).field("reason", "no_such_track");
    return;
  }

  // The DECODED source is needed for both: detection reads its audio, and every marker op
  // needs its length to validate a frame against. Read from the SNAPSHOT, which is the same
  // audio the producer plays — resolving the file again here could disagree with it.
  std::shared_ptr<const daw::SamplerRender> snap;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    snap = runtime->samplerSnapshot;
  }
  const daw::SamplerSourceAudio* audio = snap ? snap->audioFor(static_cast<uint16_t>(sourceId))
                                              : nullptr;
  if (!audio || audio->frames == 0) {
    reportSamplerReject(daw::UiCommandType::SamplerSlice, daw::UiSamplerRejectReason::NoSuchSource, trackId, 0,
                        static_cast<uint16_t>(sourceId));
    DAW_EVENT("sampler.slice_rejected")
        .field("track", trackId)
        .field("source", sourceId)
        .field("reason", "no_such_source");
    return;
  }

  uint32_t made = 0, removed = 0, slotsMade = 0;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler || (deviceId != 0 && d.id != deviceId)) {
        continue;
      }
      daw::SliceSet* set = nullptr;
      for (auto& ss : d.sampler.sliceSets) {
        if (ss.sourceLocalId == static_cast<uint16_t>(sourceId)) {
          set = &ss;
        }
      }
      if (!set) {
        daw::SliceSet fresh;
        fresh.sourceLocalId = static_cast<uint16_t>(sourceId);
        fresh.nextMarkerId = 1;
        d.sampler.sliceSets.push_back(fresh);
        set = &d.sampler.sliceSets.back();
      }
      if (isSlice) {
        std::vector<uint64_t> frames;
        switch (static_cast<daw::SamplerSliceMode>(sp.mode)) {
          case daw::SamplerSliceMode::Clear:
            removed = static_cast<uint32_t>(set->markers.size());
            set->markers.clear();
            // nextMarkerId is NOT reset. Clearing removes boundaries; it does not make the
            // retired ids safe to hand out again, and a note still naming one must stay
            // silent rather than acquiring different audio.
            break;
          case daw::SamplerSliceMode::Equal:
            frames = daw::divideEqually(audio->frames, sp.count);
            break;
          case daw::SamplerSliceMode::Transient:
          default: {
            // Detection wants ONE channel. The left is the convention here rather than a
            // downmix: a downmix can cancel a transient that is hard-panned, and losing a hit
            // to phase is a worse failure than ignoring the right channel.
            daw::SliceDetectOptions opt;
            opt.sensitivity = sp.sensitivity;
            opt.maxSlices = sp.maxSlices ? sp.maxSlices : 64;
            frames = daw::detectTransients(audio->channels[0], audio->frames, opt);
            break;
          }
        }
        if (sp.snapNanoticks > 0 && !frames.empty()) {
          // The grid arrives in NANOTICKS and the markers are in FRAMES, so it converts here
          // against this project's tempo — which is what makes the chop tempo-adaptive rather
          // than tied to the rate the file happened to be recorded at.
          const double bpm = tempoProvider.bpmAtNanotick(0);
          const double framesPerTick =
              (bpm > 0.0 ? bpm : 120.0) /
              (60.0 * static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter)) *
              audio->sampleRate;
          const uint64_t gridFrames =
              static_cast<uint64_t>(sp.snapNanoticks * framesPerTick);
          if (gridFrames > 0) {
            daw::snapToGrid(frames, gridFrames);
          }
        }
        made = daw::applySliceFrames(*set, frames, audio->frames);
        if (sp.makeSlots) {
          // ONE SLOT PER SLICE, on consecutive keys. This is the gesture that turns a chop
          // into something playable in one command rather than N — and every slot names its
          // slice by ID, so a later re-cut moves what they play without moving any row.
          uint8_t key = sp.slotBaseKey;
          // THE SOURCE'S STEM, resolved once, so every slice this chop mints is named
          // "<stem> NN". Slices were minted with NO name at all, so a chopped kit published
          // sixteen empty strings and nothing could tell slice 3 from slice 11 without
          // reading the extents. The stem rather than a bare "slice NN" because two breaks
          // chopped into one kit have to stay apart, and that is the normal case.
          std::string sliceStem;
          for (const auto& src : d.sampler.sources) {
            if (src.localId == sourceId) {
              sliceStem = sampleDisplayName(src.path);
              break;
            }
          }
          // The ordinal counts MARKERS, not slots made, so a re-cut that skips slices which
          // already have slots still numbers the new ones by where they sit in the file.
          // Numbering by slots-made would name the same slice differently depending on what
          // was chopped before it.
          uint32_t sliceOrdinal = 0;
          for (const auto& m : set->markers) {
            ++sliceOrdinal;
            bool exists = false;
            for (const auto& sl : d.sampler.slots) {
              if (sl.sliceId == m.id) {
                exists = true;
              }
            }
            if (exists || key > 127) {
              ++key;
              continue;
            }
            daw::SamplerSlot sl;
            sl.id = d.sampler.nextSlotId++;
            // The bank's default, stamped at mint. From here the slot's own gate is the
            // authority — see SamplerState::defaultGate for why this seeds and does not
            // override.
            sl.gate = d.sampler.defaultGate;
            sl.sourceLocalId = static_cast<uint16_t>(sourceId);
            sl.sliceId = m.id;
            {
              char buf[8];
              std::snprintf(buf, sizeof(buf), " %02u", sliceOrdinal);
              sl.name = sliceStem + buf;
              if (sl.name.size() >= daw::kUiSamplerSlotNameBytes) {
                sl.name = sl.name.substr(0, daw::kUiSamplerSlotNameBytes - 1);
              }
            }
            sl.keyLow = sl.keyHigh = sl.rootKey = key++;
            // FIXED PITCH: a slice played from its own key should sound as recorded, not
            // transposed by where it happens to sit on the keyboard.
            sl.pitchTrackMilli = 0;
            sl.modSetId = d.sampler.modSets.empty() ? 1 : d.sampler.modSets.front().id;
            d.sampler.slots.push_back(sl);
            ++slotsMade;
          }
        }
        ok = true;
      } else {
        switch (static_cast<daw::SamplerMarkerOp>(mp.op)) {
          case daw::SamplerMarkerOp::Add:
            ok = daw::insertSliceMarker(*set, mp.frame, audio->frames) != 0;
            made = ok ? 1 : 0;
            break;
          case daw::SamplerMarkerOp::Move:
            ok = daw::moveSliceMarker(*set, static_cast<uint16_t>(mp.markerId), mp.frame,
                                      audio->frames);
            break;
          case daw::SamplerMarkerOp::Remove:
            ok = daw::removeSliceMarker(*set, static_cast<uint16_t>(mp.markerId));
            removed = ok ? 1 : 0;
            break;
        }
      }
      break;
    }
    if (ok) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!ok) {
    reportSamplerReject(daw::UiCommandType::SamplerSlice,
                        isSlice ? daw::UiSamplerRejectReason::NotASampler : daw::UiSamplerRejectReason::BadValue,
                        trackId, deviceId, static_cast<uint16_t>(sourceId));
    DAW_EVENT("sampler.slice_rejected")
        .field("track", trackId)
        .field("device", deviceId)
        .field("source", sourceId)
        .field("reason", isSlice ? "no_sampler_device" : "marker_op_refused");
    return;
  }
  DAW_EVENT(isSlice ? "sampler.sliced" : "sampler.marker")
      .field("track", trackId)
      .field("device", deviceId)
      .field("source", sourceId)
      .field("made", made)
      .field("removed", removed)
      .field("slots", slotsMade);
  return;
}

void handleRequestSamplerEnvelope(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerEnvelopeRequestPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  if (!uiShm.header || uiShm.header->uiSamplerEnvelopeOffset == 0) {
    return;
  }
  auto* region = reinterpret_cast<daw::UiSamplerEnvelopeRegion*>(
      reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiSamplerEnvelopeOffset);
  daw::UiSamplerEnvelopeSlot& slot =
      region->slots[p.requestSeq % daw::kUiSamplerEnvelopeSlots];

  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }

  // SEQLOCK: odd while writing, as the kit and automation answers do.
  const uint32_t before = slot.seq.load(std::memory_order_relaxed) | 1u;
  slot.seq.store(before, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);

  slot.requestSeq = p.requestSeq;
  slot.trackId = p.trackId;
  slot.deviceId = p.deviceId;
  slot.modSetId = p.modSetId;
  slot.modulatorId = p.modulatorId;
  slot.target = p.target;
  slot.found = 0;
  slot.pointCount = 0;
  slot.pointsTruncated = 0;
  slot.timeBase = 0;
  slot.rateMilli = 1000;
  slot.sustainLoopStart = 255;
  slot.sustainLoopEnd = 255;
  slot.releaseLoopStart = 255;
  slot.releaseLoopEnd = 255;
  slot.releaseFade = 0;

  if (runtime) {
    // FROM THE SNAPSHOT THE PRODUCER READS, not from the document — the same decision the
    // kit read-back makes and for the same reason: the model answers "what was configured"
    // while the audio thread plays something else, and catching that divergence is the whole
    // point of a read-back.
    std::shared_ptr<const daw::SamplerRender> snap;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snap = runtime->samplerSnapshot;
    }
    if (snap && (p.deviceId == 0 || runtime->samplerDeviceId.load(std::memory_order_acquire) == p.deviceId)) {
      const bool byTarget = (p.flags & daw::kSamplerEnvByTarget) != 0;
      for (const auto& ms : snap->state.modSets) {
        if (p.modSetId != 0 && ms.id != p.modSetId) {
          continue;
        }
        for (const auto& mod : ms.modulators) {
          if (mod.kind != daw::ModKind::Envelope) {
            continue;
          }
          const bool match =
              byTarget ? (static_cast<uint8_t>(mod.target) == p.target)
                       : (p.modulatorId == 0 || mod.id == p.modulatorId);
          if (!match) {
            continue;
          }
          slot.found = 1;
          slot.deviceId = runtime->samplerDeviceId.load(std::memory_order_acquire);
          slot.modSetId = ms.id;
          slot.modulatorId = mod.id;
          slot.target = static_cast<uint8_t>(mod.target);
          slot.timeBase = mod.timeBase;
          slot.rateMilli = mod.rateMilli;
          slot.sustainLoopStart = mod.env.sustainLoopStart;
          slot.sustainLoopEnd = mod.env.sustainLoopEnd;
          slot.releaseLoopStart = mod.env.releaseLoopStart;
          slot.releaseLoopEnd = mod.env.releaseLoopEnd;
          slot.releaseFade = mod.env.releaseFade;
          uint32_t n = 0;
          for (const auto& pt : mod.env.points) {
            if (n >= daw::kUiMaxEnvelopePoints) {
              // COUNTED, not silently dropped: a truncated curve that says nothing reads as
              // the whole curve, and an editor would then SAVE the truncation back.
              ++slot.pointsTruncated;
              continue;
            }
            slot.points[n].time = pt.time;
            slot.points[n].valueMilli = pt.valueMilli;
            slot.points[n].tension = pt.tension;
            slot.points[n].flags = pt.flags;
            ++n;
          }
          slot.pointCount = static_cast<uint16_t>(n);
          break;
        }
        if (slot.found != 0) {
          break;
        }
      }
    }
  }

  std::atomic_thread_fence(std::memory_order_release);
  slot.seq.store(before + 1, std::memory_order_release);
  region->requestSeq.store(p.requestSeq, std::memory_order_release);
  DAW_EVENT("sampler.envelope_answered")
      .field("track", p.trackId)
      .field("mod_set", slot.modSetId)
      .field("modulator", static_cast<uint64_t>(slot.modulatorId))
      .field("target", static_cast<uint64_t>(slot.target))
      .field("found", slot.found)
      .field("points", static_cast<uint64_t>(slot.pointCount));
  return;
}

void handleRequestSamplerKit(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerKitRequestPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  if (!uiShm.header || uiShm.header->uiSamplerKitOffset == 0) {
    return;
  }
  auto* region = reinterpret_cast<daw::UiSamplerKitRegion*>(
      reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiSamplerKitOffset);
  // THE CLIENT OWNS THE SEQUENCE and it picks the slot, so a caller reads one place rather
  // than scanning for an answer that looks like a reply to its own question.
  daw::UiSamplerKitSlot& slot = region->slots[p.requestSeq % daw::kUiSamplerKitSlots];

  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }

  // SEQLOCK: odd while writing. A reader that sees an odd sequence, or a different one either
  // side of its read, retries — the only way a 2 KB answer publishes without a lock the
  // reader could hold while the engine needs to move on.
  const uint32_t before = slot.seq.load(std::memory_order_relaxed) | 1u;
  slot.seq.store(before, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);

  slot.requestSeq = p.requestSeq;
  slot.trackId = p.trackId;
  slot.deviceId = p.deviceId;
  slot.slotCount = 0;
  slot.slotsTruncated = 0;
  slot.found = 0;
  slot.voiceCap = 0;
  slot.activeVoices = 0;
  slot.defaultGate = 0;
  slot.defaultView = 0;
  slot.steals = 0;
  slot.unmapped = 0;

  if (runtime) {
    // PUBLISHED FROM THE SNAPSHOT THE PRODUCER READS, NOT FROM THE DOCUMENT. That is the
    // decision that gives this read-back teeth: the model would answer "what was configured"
    // while the audio thread plays something else, and catching exactly that divergence is
    // what a read-back is for.
    std::shared_ptr<const daw::SamplerRender> snap;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snap = runtime->samplerSnapshot;
    }
    if (snap && (p.deviceId == 0 || runtime->samplerDeviceId.load(std::memory_order_acquire) == p.deviceId)) {
      slot.found = 1;
      slot.deviceId = runtime->samplerDeviceId.load(std::memory_order_acquire);
      slot.voiceCap = snap->state.voiceCap;
      slot.defaultGate = snap->state.defaultGate;
      slot.defaultView = snap->state.defaultView;
      slot.activeVoices = runtime->samplerRuntime.activeVoices();
      slot.steals = static_cast<uint32_t>(runtime->samplerRuntime.stealCount());
      slot.unmapped = static_cast<uint32_t>(runtime->samplerRuntime.unmappedCount());
      // THE VERSION OF WHAT IS IN THIS ANSWER, not of what the model has reached. A reader
      // comparing this against the region's poll counter can tell "you are looking at the
      // current kit" from "the kit has moved since this was built" — which the region's
      // counter alone cannot say, because it is written on a different clock.
      slot.contentVersion = snap->version;
      uint32_t n = 0;
      for (const auto& sl : snap->state.slots) {
        if (n >= daw::kUiMaxSamplerSlots) {
          // NEVER A SILENT TRUNCATION. A kit larger than the region says so, so a UI can draw
          // "and 12 more" rather than quietly showing a short list as though it were whole.
          slot.slotsTruncated = static_cast<uint32_t>(snap->state.slots.size()) - n;
          break;
        }
        daw::UiSamplerSlotEntry& e = slot.slots[n++];
        e = daw::UiSamplerSlotEntry{};
        e.slotId = sl.id;
        e.sourceLocalId = sl.sourceLocalId;
        e.keyLow = sl.keyLow;
        e.keyHigh = sl.keyHigh;
        e.rootKey = sl.rootKey;
        e.velLow = sl.velLow;
        e.velHigh = sl.velHigh;
        e.voiceGroup = sl.voiceGroup;
        e.nna = static_cast<uint8_t>(sl.nna);
        e.flags = static_cast<uint8_t>((sl.gate ? 1u : 0u) | (sl.reverse ? 2u : 0u));
        e.gainMillibels = sl.gainMillibels;
        e.panThousandths = sl.panThousandths;
        e.modSetId = sl.modSetId;
        e.outputStem = sl.outputStem;
        e.quality = sl.quality;
        e.sliceId = sl.sliceId;
        // v36: THE NAME. Copied with a hard stop one byte short of the field so the result is
        // always nul-terminated inside its own bytes — a reader that trusts the terminator
        // must never run off the end of the entry and into the next slot's id.
        //
        // A name too long to fit CANNOT arrive here: SamplerSetSlotName refuses it. This
        // clamp is for names that predate the command — a project saved when the loader
        // stamped the full path on, which is every project made before v36.
        std::memcpy(e.name, sl.name.data(),
                    std::min(sl.name.size(), sizeof(e.name) - 1));
        // WHAT THE SLOT'S MOD SET DOES, resolved here so a UI does not have to hold the mod
        // sets to interpret a modSetId. A bit is set only when the modulator would actually
        // MOVE something: an envelope needs points, an LFO needs a non-zero swing, and both
        // need a depth. Reporting an inert modulator as a modulator is how a surface ends up
        // drawing a control that does nothing — which is what the engine itself did with pan,
        // resonance and every LFO until they were made to work.
        for (const auto& ms : snap->state.modSets) {
          if (ms.id != sl.modSetId) {
            continue;
          }
          e.filterType = ms.filterType;
          // VINTAGE, into the two reserved words — no version bump. Both are 0 today and 0
          // means OFF, so a reader that ignores them is unaffected and one that reads them
          // gets the truth for every project written before this existed.
          e.vintageBits = ms.bitDepth;
          e.vintageRateHz = ms.rateHz;
          for (const auto& m : ms.modulators) {
            if (m.depthMilli == 0) {
              continue;
            }
            const bool moves = m.kind == daw::ModKind::Envelope
                                   ? !m.env.empty()
                                   : (m.lfo.depth != 0.0f || m.lfo.bias != 0.0f);
            if (!moves) {
              continue;
            }
            const uint32_t bit =
                static_cast<uint32_t>(m.target) * 2u +
                (m.kind == daw::ModKind::Lfo ? 1u : 0u);
            if (bit < 16) {
              e.modMask |= static_cast<uint16_t>(1u << bit);
            }
          }
          break;
        }
        const daw::SamplerSourceAudio* audio = snap->audioFor(sl.sourceLocalId);
        e.lengthFrames = audio ? static_cast<uint32_t>(audio->frames) : 0;
        // THE SLICE'S EXTENT, from the same snapshot the voice reads — so what is drawn is
        // what would sound, not what the model happens to hold. A slot with no slice gets the
        // whole source rather than zeroes; see the field's comment for why that is not a
        // sentinel worth having.
        e.sliceBeginFrame = 0;
        e.sliceEndFrame = e.lengthFrames;
        if (sl.sliceId != 0 && audio != nullptr) {
          bool resolved = false;
          for (const auto& ss : snap->state.sliceSets) {
            if (ss.sourceLocalId != sl.sourceLocalId) {
              continue;
            }
            const daw::SliceExtent ext =
                daw::sliceExtentById(ss, sl.sliceId, audio->frames);
            if (ext.valid) {
              e.sliceBeginFrame = static_cast<uint32_t>(ext.begin);
              e.sliceEndFrame = static_cast<uint32_t>(ext.end);
              resolved = true;
            }
            break;
          }
          // The slot names a slice and nothing answers to that id — `--mode clear` wiped the
          // set out from under it. The whole source is what it will play; the bit is what
          // says that was not the intention. Reaching the end of the loop without a match
          // covers both shapes: the set is gone entirely, and the set is there but the id
          // is not in it.
          if (!resolved) {
            e.flags |= daw::kUiSamplerSlotSliceMissing;
          }
        }
        // "Silent because the file is missing" and "silent because the sample is empty" are
        // different problems, and a UI should be able to say which — so the reason is a FLAG
        // rather than something to infer from a zero length.
        if (!audio) {
          e.flags |= daw::kUiSamplerSlotSourceMissing;
        }
      }
      slot.slotCount = n;
    }
  }

  std::atomic_thread_fence(std::memory_order_release);
  slot.seq.store(before + 1, std::memory_order_release);
  region->requestSeq.store(p.requestSeq, std::memory_order_release);
  DAW_EVENT("sampler.kit_published")
      .field("track", p.trackId)
      .field("device", slot.deviceId)
      .field("seq", p.requestSeq)
      .field("found", slot.found)
      .field("slots", slot.slotCount)
      .field("truncated", slot.slotsTruncated)
      .field("voices", slot.activeVoices);
  return;
}

void handleSamplerSetSlot(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerSetSlotPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSetSlot, daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.set_slot_rejected")
        .field("track", p.trackId)
        .field("reason", "no_such_track");
    return;
  }
  bool applied = false;
  const char* why = "no_such_slot";
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      for (auto& slot : d.sampler.slots) {
        if (slot.id != p.slotId) {
          continue;
        }
        const int32_t v = p.value;
        // CLAMPED, NOT REFUSED, for range fields — a value out of range is almost always a
        // caller's arithmetic rather than an intent, and refusing leaves the kit in a state
        // the caller thinks it changed. Fields where a wrong value would be a DIFFERENT
        // sound rather than a clipped one (modSetId, slot ids) are validated instead.
        auto u8c = [](int32_t x) {
          return static_cast<uint8_t>(std::clamp(x, 0, 255));
        };
        auto keyc = [](int32_t x) {
          return static_cast<uint8_t>(std::clamp(x, 0, 127));
        };
        switch (static_cast<daw::SamplerSlotField>(p.field)) {
          case daw::SamplerSlotField::VoiceGroup: slot.voiceGroup = u8c(v); break;
          case daw::SamplerSlotField::Nna:
            slot.nna = static_cast<daw::SamplerNna>(std::clamp(v, 0, 2));
            break;
          case daw::SamplerSlotField::Gate: slot.gate = v ? 1 : 0; break;
          case daw::SamplerSlotField::Reverse: slot.reverse = v ? 1 : 0; break;
          case daw::SamplerSlotField::GainMillibels:
            slot.gainMillibels = static_cast<int16_t>(std::clamp(v, -9600, 2400));
            break;
          case daw::SamplerSlotField::PanThousandths:
            slot.panThousandths = static_cast<int16_t>(std::clamp(v, -1000, 1000));
            break;
          case daw::SamplerSlotField::TuneCents:
            slot.tuneCents = static_cast<int16_t>(std::clamp(v, -4800, 4800));
            break;
          case daw::SamplerSlotField::PitchTrackMilli:
            slot.pitchTrackMilli = static_cast<int16_t>(std::clamp(v, -2000, 2000));
            break;
          case daw::SamplerSlotField::RootKey: slot.rootKey = keyc(v); break;
          case daw::SamplerSlotField::KeyLow: slot.keyLow = keyc(v); break;
          case daw::SamplerSlotField::KeyHigh: slot.keyHigh = keyc(v); break;
          case daw::SamplerSlotField::VelLow: slot.velLow = keyc(v); break;
          case daw::SamplerSlotField::VelHigh: slot.velHigh = keyc(v); break;
          case daw::SamplerSlotField::SelectMode:
            slot.selectMode = static_cast<uint8_t>(std::clamp(v, 0, 3));
            break;
          case daw::SamplerSlotField::Polyphony: slot.polyphony = u8c(v); break;
          // THE LOOP AND THE TRIM. Frames are clamped at 0 rather than refused: a negative
          // frame is not a position, and the int32 wire cannot express one that is too big.
          case daw::SamplerSlotField::LoopMode:
            slot.loopMode = static_cast<uint8_t>(std::clamp(v, 0, 3));
            break;
          case daw::SamplerSlotField::SustainLoop:
            slot.sustainLoop = v ? 1 : 0;
            break;
          case daw::SamplerSlotField::LoopStartFrame:
            slot.loopStartFrame = static_cast<uint64_t>(std::max(v, 0));
            break;
          case daw::SamplerSlotField::LoopEndFrame:
            slot.loopEndFrame = static_cast<uint64_t>(std::max(v, 0));
            break;
          case daw::SamplerSlotField::LoopXfadeFrames:
            slot.loopXfadeFrames = static_cast<uint64_t>(std::max(v, 0));
            break;
          case daw::SamplerSlotField::StartFrame:
            slot.startFrame = static_cast<uint64_t>(std::max(v, 0));
            break;
          case daw::SamplerSlotField::EndFrame:
            slot.endFrame = static_cast<uint64_t>(std::max(v, 0));
            break;
          case daw::SamplerSlotField::ChokeFadeUs:
            slot.chokeFadeUs = static_cast<uint32_t>(std::clamp(v, 0, 1000000));
            break;
          case daw::SamplerSlotField::ModSetId: {
            // A mod set that does not exist would leave the slot with NO amp envelope, so
            // it would go silent — refused rather than clamped, because "silent" is not a
            // near-miss of what the caller asked for.
            const uint16_t want = static_cast<uint16_t>(std::max(0, v));
            if (!d.sampler.findModSet(want)) {
              why = "no_such_mod_set";
              goto done;
            }
            slot.modSetId = want;
            break;
          }
          case daw::SamplerSlotField::SourceLocalId: {
            // A source that is not there would make the slot silent — refused, like ModSetId.
            const uint16_t want = static_cast<uint16_t>(std::max(0, v));
            if (want == 0 || !d.sampler.findSource(want)) {
              why = "no_such_source";
              goto done;
            }
            slot.sourceLocalId = want;
            break;
          }
          case daw::SamplerSlotField::SliceId: {
            const uint16_t want = static_cast<uint16_t>(std::max(0, v));
            if (want != 0) {
              // The marker must exist in THIS SLOT'S source's slice set. A slice id is only
              // meaningful against the sample it was cut from, so validating it globally
              // would accept slice 7 of another file and play the wrong region.
              bool found = false;
              for (const auto& ss : d.sampler.sliceSets) {
                if (ss.sourceLocalId != slot.sourceLocalId) {
                  continue;
                }
                for (const auto& m : ss.markers) {
                  if (m.id == want) {
                    found = true;
                    break;
                  }
                }
                break;
              }
              if (!found) {
                why = "no_such_slice";
                goto done;
              }
            }
            slot.sliceId = want;
            break;
          }
          case daw::SamplerSlotField::OutputStem: slot.outputStem = u8c(v); break;
          case daw::SamplerSlotField::Quality:
            slot.quality = static_cast<uint8_t>(std::clamp(v, 0, 2));
            break;
          case daw::SamplerSlotField::LayerGroup:
            slot.layerGroup = static_cast<uint16_t>(std::clamp(v, 0, 65535));
            break;
          default:
            why = "unknown_field";
            goto done;
        }
        applied = true;
        goto done;
      }
    }
  done:
    if (applied) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!applied) {
    reportSamplerReject(daw::UiCommandType::SamplerSetSlot, samplerReasonFor(why),
                        p.trackId, p.deviceId, static_cast<uint16_t>(p.slotId));
    DAW_EVENT("sampler.set_slot_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("slot", p.slotId)
        .field("field", static_cast<uint32_t>(p.field))
        .field("reason", why);
    return;
  }
  DAW_EVENT("sampler.slot_set")
      .field("track", p.trackId)
      .field("device", p.deviceId)
      .field("slot", p.slotId)
      .field("field", static_cast<uint32_t>(p.field))
      .field("value", static_cast<int64_t>(p.value));
  return;
}

void handleSamplerSetDevice(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerSetDevicePayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSetDevice,
                        daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.set_device_rejected")
        .field("track", p.trackId)
        .field("reason", "no_such_track");
    return;
  }
  bool applied = false;
  const char* why = "no_sampler_device";
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      switch (static_cast<daw::SamplerDeviceField>(p.field)) {
        case daw::SamplerDeviceField::DefaultGate:
          d.sampler.defaultGate = p.value ? 1 : 0;
          break;
        // VOICE CAP IS REFUSED AT ZERO, not clamped to it. A cap of 0 is a device that can
        // never sound, which is not a near-miss of anything a caller meant — and it is
        // exactly the kind of value that reads as "the sampler is broken" rather than "I
        // sent a bad number". The upper bound is a clamp because 500 voices IS a caller
        // asking for as many as they can have.
        case daw::SamplerDeviceField::VoiceCap: {
          if (p.value <= 0) {
            why = "bad_value";
            goto devdone;
          }
          d.sampler.voiceCap = static_cast<uint8_t>(std::clamp(p.value, 1, 255));
          break;
        }
        case daw::SamplerDeviceField::DefaultView:
          d.sampler.defaultView = p.value ? 1 : 0;
          break;
        default:
          why = "unknown_field";
          goto devdone;
      }
      applied = true;
      goto devdone;
    }
  devdone:
    // REFRESHED, and my first version of this deliberately did not — with the reasoning that
    // none of these three changes anything a VOICE reads, so rebuilding would retire every
    // live voice's snapshot for a setting that cannot be heard. That was right about the
    // voices and wrong about the READ-BACK: the kit answer is built from the RT snapshot, so
    // a model-only change is invisible to it and default_gate read back as 0 immediately
    // after being set to 1.
    //
    // Publishing this one field from the model instead would put TWO CLOCKS in one payload —
    // the model's value beside the snapshot's slots — which is exactly the defect #96 was:
    // an answer whose parts describe different moments. One source, one clock, one refresh.
    if (applied) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!applied) {
    reportSamplerReject(daw::UiCommandType::SamplerSetDevice,
                        std::strcmp(why, "bad_value") == 0
                            ? daw::UiSamplerRejectReason::BadValue
                            : (std::strcmp(why, "unknown_field") == 0
                                   ? daw::UiSamplerRejectReason::BadValue
                                   : daw::UiSamplerRejectReason::NotASampler),
                        p.trackId, p.deviceId, static_cast<uint16_t>(p.field));
    DAW_EVENT("sampler.set_device_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("field", static_cast<uint32_t>(p.field))
        .field("reason", why);
    return;
  }
  DAW_EVENT("sampler.device_set")
      .field("track", p.trackId)
      .field("device", p.deviceId)
      .field("field", static_cast<uint32_t>(p.field))
      .field("value", static_cast<int64_t>(p.value));
  return;
}

void handleSamplerSetFilter(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerFilterPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSetFilter, daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.filter_rejected")
        .field("track", p.trackId)
        .field("reason", "no_such_track");
    return;
  }
  // OUT OF RANGE IS REFUSED, NOT CLAMPED. A filter type is an enumeration, not a continuous
  // control someone sweeps — 7 is not "a bit past BP", it is a caller with the wrong idea of
  // the encoding, and clamping it to BP would hand them a filter they did not ask for and no
  // way to discover the mistake.
  if (p.filterType > 4) {
    reportSamplerReject(daw::UiCommandType::SamplerSetFilter, daw::UiSamplerRejectReason::BadValue, p.trackId, 0,
                        static_cast<uint16_t>(p.filterType));
    DAW_EVENT("sampler.filter_rejected")
        .field("track", p.trackId)
        .field("type", static_cast<uint32_t>(p.filterType))
        .field("reason", "no_such_filter_type");
    return;
  }
  bool applied = false;
  uint32_t touched = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      ensureDefaultModSet(d.sampler, p.modSetId);
      for (auto& ms : d.sampler.modSets) {
        if (p.modSetId != 0 && ms.id != p.modSetId) {
          continue;
        }
        ms.filterType = p.filterType;
        // The two flags are what makes "set the type, leave the cutoff" expressible. Zero is
        // a legal cutoff, so absence cannot be encoded as a zero value.
        if ((p.flags & daw::kSamplerFilterSetCutoff) != 0) {
          ms.cutoffMilli = static_cast<uint16_t>(std::min<uint32_t>(p.cutoffMilli, 1000));
        }
        if ((p.flags & daw::kSamplerFilterSetResonance) != 0) {
          ms.resonanceMilli =
              static_cast<uint16_t>(std::min<uint32_t>(p.resonanceMilli, 1000));
        }
        applied = true;
        ++touched;
      }
      if (applied) {
        break;
      }
    }
    if (applied) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!applied) {
    reportSamplerReject(daw::UiCommandType::SamplerSetFilter, daw::UiSamplerRejectReason::NoSuchModSet, p.trackId, p.deviceId,
                        static_cast<uint16_t>(p.modSetId));
    DAW_EVENT("sampler.filter_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("mod_set", p.modSetId)
        .field("reason", "no_such_mod_set");
    return;
  }
  DAW_EVENT("sampler.filter_set")
      .field("track", p.trackId)
      .field("device", p.deviceId)
      .field("mod_set", p.modSetId)
      .field("mod_sets_touched", touched)
      .field("type", static_cast<uint32_t>(p.filterType))
      .field("cutoff_milli", static_cast<uint32_t>(p.cutoffMilli))
      .field("resonance_milli", static_cast<uint32_t>(p.resonanceMilli));
  return;
}

void handleSamplerSetVintage(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerVintagePayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSetVintage,
                        daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.vintage_rejected")
        .field("track", p.trackId)
        .field("reason", "no_such_track");
    return;
  }
  // REFUSED, NOT CLAMPED. 24 bits is not "a bit past 16" — the field means 2^n levels and
  // anything above 16 is a caller with the wrong idea of the unit, exactly as an unknown
  // filter type is. Clamping would hand back a sound they did not ask for with no way to
  // notice.
  if ((p.flags & daw::kSamplerVintageSetBits) != 0 && p.bitDepth > 16) {
    reportSamplerReject(daw::UiCommandType::SamplerSetVintage,
                        daw::UiSamplerRejectReason::BadValue, p.trackId, p.deviceId,
                        static_cast<uint16_t>(p.bitDepth));
    DAW_EVENT("sampler.vintage_rejected")
        .field("track", p.trackId)
        .field("bits", static_cast<uint32_t>(p.bitDepth))
        .field("reason", "bit_depth_out_of_range");
    return;
  }
  bool applied = false;
  uint32_t touched = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      ensureDefaultModSet(d.sampler, p.modSetId);
      for (auto& ms : d.sampler.modSets) {
        if (p.modSetId != 0 && ms.id != p.modSetId) {
          continue;
        }
        // The flags are what makes "set the bits, leave the rate" expressible. Zero is a
        // legal value for both — it means OFF — so absence cannot be encoded as a zero.
        if ((p.flags & daw::kSamplerVintageSetBits) != 0) {
          ms.bitDepth = p.bitDepth;
        }
        if ((p.flags & daw::kSamplerVintageSetRate) != 0) {
          ms.rateHz = p.rateHz;
        }
        applied = true;
        ++touched;
      }
      if (applied) {
        break;
      }
    }
    if (applied) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!applied) {
    reportSamplerReject(daw::UiCommandType::SamplerSetVintage,
                        daw::UiSamplerRejectReason::NoSuchModSet, p.trackId, p.deviceId,
                        static_cast<uint16_t>(p.modSetId));
    DAW_EVENT("sampler.vintage_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("mod_set", p.modSetId)
        .field("reason", "no_such_mod_set");
    return;
  }
  DAW_EVENT("sampler.vintage_set")
      .field("track", p.trackId)
      .field("device", p.deviceId)
      .field("mod_set", p.modSetId)
      .field("mod_sets_touched", touched)
      .field("bits", static_cast<uint32_t>(p.bitDepth))
      .field("rate_hz", static_cast<uint32_t>(p.rateHz));
  return;
}

void handleSamplerSetLfo(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerLfoPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSetLfo, daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.lfo_rejected")
        .field("track", p.trackId)
        .field("reason", "no_such_track");
    return;
  }
  bool applied = false;
  uint16_t targetId = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      ensureDefaultModSet(d.sampler, p.modSetId);
      for (auto& ms : d.sampler.modSets) {
        if (p.modSetId != 0 && ms.id != p.modSetId) {
          continue;
        }
        daw::SamplerModulator* mod = nullptr;
        const auto target =
            static_cast<daw::ModTarget>(std::min<uint8_t>(p.target, 4));
        if ((p.flags & daw::kSamplerEnvByTarget) != 0) {
          for (auto& m : ms.modulators) {
            if (m.kind == daw::ModKind::Lfo && m.target == target) {
              mod = &m;
              break;
            }
          }
          if (mod == nullptr) {
            daw::SamplerModulator fresh;
            fresh.id = ms.nextModulatorId++;
            fresh.kind = daw::ModKind::Lfo;
            fresh.target = target;
            fresh.apply = target == daw::ModTarget::Volume ? 1 : 0;
            ms.modulators.push_back(fresh);
            mod = &ms.modulators.back();
          }
        } else {
          for (auto& m : ms.modulators) {
            if (m.id == p.modulatorId) {
              mod = &m;
              break;
            }
          }
        }
        if (mod == nullptr) {
          break;
        }
        mod->kind = daw::ModKind::Lfo;
        mod->target = target;
        // NEGATIVE OR ABSURD RATES ARE REFUSED BY CLAMP, not by rejection: a frequency is a
        // continuous control someone will sweep, and refusing mid-sweep is worse than
        // stopping at the end of the range. 0.01..200 Hz spans a bar-long swell to an
        // audible-rate buzz, which is the whole musical range of the thing.
        mod->lfo.frequency_hz = std::clamp(p.frequencyHz, 0.01f, 200.0f);
        mod->lfo.depth = std::clamp(p.depth, -4.0f, 4.0f);
        mod->lfo.bias = std::clamp(p.bias, -4.0f, 4.0f);
        mod->lfo.phase_offset = p.phaseOffset;
        mod->depthMilli = std::clamp<int16_t>(p.depthMilli, -1000, 1000);
        targetId = mod->id;
        applied = true;
        break;
      }
      if (applied) {
        break;
      }
    }
    if (applied) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!applied) {
    reportSamplerReject(daw::UiCommandType::SamplerSetLfo, daw::UiSamplerRejectReason::NoSuchModSet, p.trackId, 0,
                        static_cast<uint16_t>(p.modSetId));
    DAW_EVENT("sampler.lfo_rejected")
        .field("track", p.trackId)
        .field("mod_set", p.modSetId)
        .field("reason", "no_such_mod_set_or_modulator");
    return;
  }
  DAW_EVENT("sampler.lfo_set")
      .field("track", p.trackId)
      .field("modulator", static_cast<uint32_t>(targetId))
      .field("target", static_cast<uint32_t>(p.target))
      .field("hz_milli", static_cast<uint64_t>(p.frequencyHz * 1000.0f))
      .field("depth_milli", static_cast<int64_t>(p.depthMilli));
  return;
}

void handleSamplerSetEnvelope(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerEnvelopePayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime) {
    reportSamplerReject(daw::UiCommandType::SamplerSetEnvelope, daw::UiSamplerRejectReason::NoSuchTrack, p.trackId, 0, 0);
    DAW_EVENT("sampler.envelope_rejected")
        .field("track", p.trackId)
        .field("reason", "no_such_track");
    return;
  }
  bool applied = false;
  const char* why = "no_such_mod_set";
  uint16_t targetId = 0;
  daw::EnvRepair repair;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      ensureDefaultModSet(d.sampler, p.modSetId);
      for (auto& ms : d.sampler.modSets) {
        if (p.modSetId != 0 && ms.id != p.modSetId) {
          continue;
        }
        daw::SamplerModulator* mod = nullptr;
        if ((p.flags & daw::kSamplerEnvByTarget) != 0) {
          mod = findOrMintEnvelope(
              ms, static_cast<daw::ModTarget>(std::min<uint8_t>(p.target, 4)));
        } else {
          for (auto& m : ms.modulators) {
            if (m.id == p.modulatorId) {
              mod = &m;
              break;
            }
          }
          if (mod == nullptr) {
            why = "no_such_modulator";
            break;
          }
        }
        mod->kind = daw::ModKind::Envelope;
        mod->env = daw::makeAdsr(p.attack, p.decay,
                                 std::clamp<int32_t>(p.sustainMilli, 0, 1000),
                                 p.release);
        mod->timeBase = p.timeBase != 0 ? 1 : 0;
        // 250..4000 matches the field's documented range. Zero would divide by zero in the
        // runner's unit conversion, so it is not merely out of range but unusable.
        mod->rateMilli = static_cast<uint16_t>(
            std::clamp<int32_t>(p.rateMilli == 0 ? 1000 : p.rateMilli, 250, 4000));
        // DEPTH IS WHAT THE TARGET NEEDS. On Volume the shape is the whole story and full
        // depth is right; on Cutoff a depth of 1000 is +-6 octaves and a shallower sweep is
        // usually what is wanted, so the caller says. Signed: a negative depth inverts.
        mod->depthMilli = std::clamp<int16_t>(p.depthMilli, -1000, 1000);
        // The ADSR editor, explicitly. Never inferred from the shape — see the field's
        // comment: sniffing "four points with a sustain loop?" would flip the editor out
        // from under someone who hand-drew a four-point curve.
        mod->editor = 0;
        // Reported, never silent — see the pencil path. An ADSR can be repaired too: a
        // sustain of 0 with attack+decay+release all 0 collapses four points onto one time,
        // and the user should be told their envelope was nudged rather than left wondering.
        repair = daw::repairEnvShape(mod->env);
        targetId = mod->id;
        applied = true;
        break;
      }
      if (applied || why != nullptr) {
        break;
      }
    }
    if (applied) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!applied) {
    reportSamplerReject(daw::UiCommandType::SamplerSetEnvelope, samplerReasonFor(why),
                        p.trackId, p.deviceId, static_cast<uint16_t>(p.modSetId));
    DAW_EVENT("sampler.envelope_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("mod_set", p.modSetId)
        .field("reason", why);
    return;
  }
  DAW_EVENT("sampler.envelope_set")
      .field("track", p.trackId)
      .field("mod_set", p.modSetId)
      .field("modulator", static_cast<uint32_t>(targetId))
      .field("attack", static_cast<uint64_t>(p.attack))
      .field("decay", static_cast<uint64_t>(p.decay))
      .field("sustain_milli", static_cast<int64_t>(p.sustainMilli))
      .field("release", static_cast<uint64_t>(p.release))
      .field("time_base", static_cast<uint32_t>(p.timeBase));
  if (repair.any()) {
    DAW_EVENT("sampler.envelope_repaired")
        .field("track", p.trackId)
        .field("modulator", static_cast<uint32_t>(targetId))
        .field("reordered", repair.reorderedPoints)
        .field("dropped", repair.droppedPoints)
        .field("cleared_sustain_loop", repair.clearedSustainLoop ? 1u : 0u)
        .field("cleared_release_loop", repair.clearedReleaseLoop ? 1u : 0u)
        .field("swapped_sustain_loop", repair.swappedSustainLoop ? 1u : 0u)
        .field("swapped_release_loop", repair.swappedReleaseLoop ? 1u : 0u)
        .field("added_release_fade", repair.addedReleaseFade ? 1u : 0u);
  }
  return;
}

void handleSamplerLoad(SamplerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  // The bodies below are VERBATIM from handleUiEntry. These aliases are what make that possible:
  // every name they used from main's scope resolves here to the same object, by reference. Not a
  // copy, and not a rename — a rename across 1,400 moved lines is exactly the kind of edit whose
  // mistakes survive review.
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& tempoProvider = deps.tempoProvider;
  const auto& reportSamplerReject = deps.reportSamplerReject;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  const auto& rebuildSamplerRender = deps.rebuildSamplerRender;
  const auto& applyAddNote = deps.applyAddNote;
  (void)uiShm; (void)tracks; (void)tracksMutex; (void)tempoProvider; (void)reportSamplerReject;
  (void)refreshSamplerForTrack; (void)rebuildSamplerRender; (void)applyAddNote;
  (void)entry; (void)header; (void)commandType;
  daw::UiSamplerLoadPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  const std::string name(p.name, strnlen(p.name, sizeof(p.name)));
  TrackRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (p.trackId < tracks.size()) {
      runtime = tracks[p.trackId].get();
    }
  }
  if (!runtime || name.empty()) {
    reportSamplerReject(daw::UiCommandType::SamplerLoad,
                        name.empty() ? daw::UiSamplerRejectReason::BadValue : daw::UiSamplerRejectReason::NoSuchTrack,
                        p.trackId, p.deviceId, 0);
    DAW_EVENT("sampler.load_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("reason", name.empty() ? "empty_name" : "no_such_track");
    return;
  }
  uint16_t newSlot = 0, newSource = 0;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::Sampler ||
          (p.deviceId != 0 && d.id != p.deviceId)) {
        continue;
      }
      found = true;
      d.hasSampler = true;
      if (d.sampler.modSets.empty()) {
        d.sampler.modSets.push_back(daw::defaultModSet(1));
        d.sampler.nextModSetId = 2;
      }
      // ONE SOURCE PER FILE. Loading the same file twice reuses the source rather than
      // decoding it again — two slots pointing at one source is the normal case (a slice
      // set is exactly that), and a duplicate would double the memory for no benefit.
      for (const auto& src : d.sampler.sources) {
        if (src.path == name) {
          newSource = src.localId;
          break;
        }
      }
      if (newSource == 0) {
        daw::SamplerSource src;
        src.localId = d.sampler.nextSourceId++;
        src.path = name;
        d.sampler.sources.push_back(src);
        newSource = src.localId;
      }
      daw::SamplerSlot slot;
      slot.id = d.sampler.nextSlotId++;
      slot.gate = d.sampler.defaultGate;  // the bank's default, stamped at mint
      // The file's STEM, not `name` — which is the full path, and is what the SOURCE keeps.
      // A seed, not an override: SamplerSetSlotName is the authority from here.
      slot.name = sampleDisplayName(name);
      slot.sourceLocalId = newSource;
      slot.rootKey = p.rootKey;
      // The mapping is DERIVED from the keys, so this writes keys rather than a mode.
      if (p.flags & daw::kSamplerLoadFixedPitch) {
        slot.keyLow = slot.keyHigh = p.rootKey;
      } else {
        slot.keyLow = 0;
        slot.keyHigh = 127;
      }
      slot.modSetId = d.sampler.modSets.front().id;
      d.sampler.slots.push_back(slot);
      newSlot = slot.id;
      break;
    }
    if (found) {
      refreshSamplerForTrack(*runtime);
    }
  }
  if (!found) {
    reportSamplerReject(daw::UiCommandType::SamplerLoad, daw::UiSamplerRejectReason::NotASampler, p.trackId, p.deviceId, 0);
    DAW_EVENT("sampler.load_rejected")
        .field("track", p.trackId)
        .field("device", p.deviceId)
        .field("reason", "no_sampler_device");
    return;
  }
  // Whether the FILE resolved is reported by rebuildSamplerRender (sampler.source_missing /
  // sampler.render_built), so a slot that will be silent says so at load rather than at
  // playback. The slot is still created either way: a broken reference you can see and fix
  // beats a command that quietly did nothing.
  DAW_EVENT("sampler.loaded")
      .field("track", p.trackId)
      .field("device", p.deviceId)
      .field("slot", static_cast<uint32_t>(newSlot))
      .field("source", static_cast<uint32_t>(newSource))
      .field("root", static_cast<uint32_t>(p.rootKey))
      .field("fixed_pitch", (p.flags & daw::kSamplerLoadFixedPitch) ? 1u : 0u)
      .field("file", name);
  return;
}

}  // namespace daw::engine
