#include "engine_handle_ui_entry.h"

// The module header carries the dependency surface of the Deps struct — the command
// modules, engine_types, the event log. These two are what the dispatcher BODY reaches
// for on its own, and they are the whole list: the file arrived here carrying main.cpp's
// 92 includes, which described where it used to live rather than what it uses.
#include "engine_rt_helpers.h"
#include "ripple.h"


namespace daw::engine {

// A placement edit sends this in a field it does not want to change. It was a constexpr
// local of main(), which is why the capture enumeration never mentioned it: a constant
// expression needs no capture. main() has no other use for it, so it moved here whole
// rather than being duplicated.
constexpr uint64_t kPlacementUnchanged = 0xFFFFFFFFFFFFFFFFull;

void handleUiEntry(HandleUiEntryDeps& deps, const daw::EventEntry& entry) {
  // Re-bind every dependency to the name the body already uses. This is what lets the
  // 1,623 lines below be the untouched original.
  auto& applyPlacementEdit = deps.applyPlacementEdit;
  auto& arrangeMutex = deps.arrangeMutex;
  auto& arrangeVersion = deps.arrangeVersion;
  auto& automationCommandDeps = deps.automationCommandDeps;
  auto& automationVersion = deps.automationVersion;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& bulkStreams = deps.bulkStreams;
  auto& bulkTick = deps.bulkTick;
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& chainCommandDeps = deps.chainCommandDeps;
  auto& clipCommandDeps = deps.clipCommandDeps;
  auto& clipDirty = deps.clipDirty;
  auto& clipVersion = deps.clipVersion;
  auto& deviceCommandDeps = deps.deviceCommandDeps;
  auto& enqueuePreview = deps.enqueuePreview;
  auto& handleAssembledBulk = deps.handleAssembledBulk;
  auto& harmonyDirty = deps.harmonyDirty;
  auto& harmonyEvents = deps.harmonyEvents;
  auto& harmonyMutex = deps.harmonyMutex;
  auto& harmonyVersion = deps.harmonyVersion;
  auto& heldPreview = deps.heldPreview;
  auto& historyAppend = deps.historyAppend;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& loadedTempoMap = deps.loadedTempoMap;
  auto& loopEndNanotick = deps.loopEndNanotick;
  auto& loopStartNanotick = deps.loopStartNanotick;
  auto& loopUserSet = deps.loopUserSet;
  auto& markerCommandDeps = deps.markerCommandDeps;
  auto& markerList = deps.markerList;
  auto& masterTrack = deps.masterTrack;
  auto& meterSnapshot = deps.meterSnapshot;
  auto& modlinkCommandDeps = deps.modlinkCommandDeps;
  auto& moduleCommandDeps = deps.moduleCommandDeps;
  auto& nextClipId = deps.nextClipId;
  auto& nextPlacementId = deps.nextPlacementId;
  auto& noteCommandDeps = deps.noteCommandDeps;
  auto& panicPending = deps.panicPending;
  auto& patcherCommandDeps = deps.patcherCommandDeps;
  auto& patternTicks = deps.patternTicks;
  auto& pendingPreviewNotes = deps.pendingPreviewNotes;
  auto& playing = deps.playing;
  auto& previewMutex = deps.previewMutex;
  auto& projectCommandDeps = deps.projectCommandDeps;
  auto& pushStructuralUndo = deps.pushStructuralUndo;
  auto& pushUndo = deps.pushUndo;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& recomputeSongEnd = deps.recomputeSongEnd;
  auto& requestCommandDeps = deps.requestCommandDeps;
  auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  auto& resetTimeline = deps.resetTimeline;
  auto& resetTrackContent = deps.resetTrackContent;
  auto& restartCv = deps.restartCv;
  auto& restartTrackHost = deps.restartTrackHost;
  auto& rowopsCommandDeps = deps.rowopsCommandDeps;
  auto& running = deps.running;
  auto& samplerCommandDeps = deps.samplerCommandDeps;
  auto& setupTrackRuntime = deps.setupTrackRuntime;
  auto& snapshotSongStore = deps.snapshotSongStore;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& songMeter = deps.songMeter;
  auto& songTimeSigDen = deps.songTimeSigDen;
  auto& songTimeSigNum = deps.songTimeSigNum;
  auto& tempoProvider = deps.tempoProvider;
  auto& trackCommandDeps = deps.trackCommandDeps;
  auto& trackpropsCommandDeps = deps.trackpropsCommandDeps;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& transportElapsedNanotick = deps.transportElapsedNanotick;
  auto& transportNanotick = deps.transportNanotick;
  auto& undoCommandDeps = deps.undoCommandDeps;

    if (entry.type != static_cast<uint16_t>(daw::EventType::UiCommand)) {
      return;
    }
    if (entry.size < sizeof(daw::UiCommandPayload)) {
      return;
    }
    daw::UiCommandPayload header{};
    std::memcpy(&header, entry.payload, sizeof(header));
    const auto commandType =
        static_cast<daw::UiCommandType>(header.commandType);

    // ---- BULK CHUNK (83). Intercepted BEFORE the journal: a 17-chunk envelope would otherwise
    // write 17 indistinguishable lines and bury the command it spells. The ASSEMBLED command
    // journals itself.
    if (entry.size == sizeof(daw::UiBulkChunkPayload) &&
        commandType == daw::UiCommandType::BulkChunk) {
      daw::UiBulkChunkPayload c{};
      std::memcpy(&c, entry.payload, sizeof(c));
      if (c.total == 0 || c.total > daw::kBulkMaxChunks || c.seq >= c.total) {
        DAW_EVENT("bulk.rejected")
            .field("stream", static_cast<uint32_t>(c.streamId))
            .field("seq", static_cast<uint32_t>(c.seq))
            .field("total", static_cast<uint32_t>(c.total))
            .field("reason", "bad_chunk_header");
        return;
      }
      ++bulkTick;
      BulkStream* stream = nullptr;
      for (auto& s : bulkStreams) {
        if (s.streamId == c.streamId && s.total == c.total) {
          stream = &s;
          break;
        }
      }
      if (stream == nullptr) {
        // BOUNDED. A sender that dies mid-message costs a buffer until it is evicted, not a
        // leak — so the oldest partial stream goes rather than the newest being refused, which
        // would let one abandoned stream block the carrier for everyone.
        if (bulkStreams.size() >= daw::kBulkMaxStreams) {
          size_t oldest = 0;
          for (size_t i = 1; i < bulkStreams.size(); ++i) {
            if (bulkStreams[i].lastTouched < bulkStreams[oldest].lastTouched) {
              oldest = i;
            }
          }
          DAW_EVENT("bulk.evicted")
              .field("stream", static_cast<uint32_t>(bulkStreams[oldest].streamId))
              .field("received", bulkStreams[oldest].received)
              .field("total", static_cast<uint32_t>(bulkStreams[oldest].total));
          bulkStreams.erase(bulkStreams.begin() + static_cast<long>(oldest));
        }
        BulkStream fresh;
        fresh.streamId = c.streamId;
        fresh.total = c.total;
        fresh.seen.assign(c.total, false);
        fresh.data.assign(static_cast<size_t>(c.total) * daw::kBulkChunkBytes, 0);
        bulkStreams.push_back(std::move(fresh));
        stream = &bulkStreams.back();
      }
      stream->lastTouched = bulkTick;
      // A REPEATED chunk is not a second chunk. Counting it would complete a stream that is
      // still missing a piece, and deliver a message with a hole in it.
      if (!stream->seen[c.seq]) {
        stream->seen[c.seq] = true;
        ++stream->received;
        std::memcpy(stream->data.data() + static_cast<size_t>(c.seq) * daw::kBulkChunkBytes,
                    c.bytes, daw::kBulkChunkBytes);
      }
      if (stream->received == stream->total) {
        std::vector<uint8_t> assembled = std::move(stream->data);
        const uint16_t doneId = stream->streamId;
        bulkStreams.erase(bulkStreams.begin() +
                          static_cast<long>(stream - bulkStreams.data()));
        DAW_EVENT("bulk.assembled")
            .field("stream", static_cast<uint32_t>(doneId))
            .field("chunks", static_cast<uint32_t>(c.total))
            .field("bytes", static_cast<uint64_t>(assembled.size()));
        handleAssembledBulk(assembled);
      }
      return;
    }
    // Journal every command the engine acts on, in order. Recorded here — the one point
    // every command passes through — rather than at ~20 handlers, so a new opcode cannot
    // silently escape the journal. Outcome is "received"; a command later refused by the
    // version check writes its own "rejected" line, so history shows the attempt AND its
    // fate rather than quietly dropping it.
    {
      const bool globalScope = daw::uiCommandIsGlobalScope(commandType);
      std::ostringstream params;
      if (daw::uiCommandUsesGenericPayload(commandType)) {
        // value0 is signed for at least one op (mixer gain in millibels), so render it
        // signed: an unsigned -600 reads as 4294966696, which looks like corruption.
        params << "\"value0\":" << static_cast<int32_t>(header.value0)
               << ",\"pitch\":" << header.notePitch << ",\"flags\":" << header.flags
               << ",\"nanotick\":"
               << ((static_cast<uint64_t>(header.noteNanotickHi) << 32) |
                   header.noteNanotickLo)
               << ",\"duration\":"
               << ((static_cast<uint64_t>(header.noteDurationHi) << 32) |
                   header.noteDurationLo);
      }
      historyAppend(daw::uiCommandTypeName(commandType), "received",
                    globalScope ? 0xFFFFFFFFu : header.trackId,
                    header.baseVersion, params.str());
    }
    if (entry.size == sizeof(daw::UiAutomationCommandPayload) &&
        commandType == daw::UiCommandType::SetAutomationTarget) {
      daw::engine::handleSetAutomationTarget(automationCommandDeps, entry, header, commandType);
      return;
    }
    // v28: ANSWER one automation lane's points into a seqlock slot. Same shape as the windowed
    // waveform queries: the client picks a slot by its request sequence, the engine fills it and
    // releases the seqlock, and every request field is ECHOED so a caller can tell WHICH question
    // this is the answer to — without that, a slot reused for a different lane looks like an
    // answer to the one you asked.
    if (entry.size == sizeof(daw::UiAutomationLaneRequestPayload) &&
        commandType == daw::UiCommandType::RequestAutomationLane) {
      daw::engine::handleRequestAutomationLane(automationCommandDeps, entry, header, commandType);
      return;
    }
    // M3.27: write an automation point. Automation playback has been built and tested
    // since M3 phase 1, but nothing ever CREATED a clip — this is the missing half.
    if (entry.size == sizeof(daw::UiAutomationPointPayload) &&
        commandType == daw::UiCommandType::WriteAutomationPoint) {
      daw::engine::handleWriteAutomationPoint(automationCommandDeps, entry, header, commandType);
      return;
    }

    // ---- DELETE AN AUTOMATION POINT (96). The other direction of the same edit.
    //
    // Opcode 60 creates a point and changes the value of one, and nothing removed one — so an
    // automation lane was draw-only: a point written at the wrong tick could be neutralised by
    // writing another beside it, and the mistake stayed in the curve. Reported by the web-UI
    // agent as the reason their automation lane has no eraser.
    //
    // Addressed exactly as the write is, and sharing its payload: same trackId, same
    // targetPluginIndex, same paramId, same tick. `value` is ignored.
    if (entry.size == sizeof(daw::UiAutomationPointPayload) &&
        commandType == daw::UiCommandType::DeleteAutomationPoint) {
      daw::engine::handleDeleteAutomationPoint(automationCommandDeps, entry, header, commandType);
      return;
    }
    // M3.23 SECTION ops. All five are SONG-scoped: the spine belongs to no track, and
    // SetSectionLength moves placements on every track at once.
    // v29 MARKER ops — naming a position. TOTAL: they move no material, so there is nothing to
    // plan, refuse or undo beyond the list itself. That separation is the whole design: every
    // section op used to have two possible meanings (re-partition the labels, or insert and remove
    // arrangement time) and implemented one of each, so a boundary drag moved the music while
    // adding a section silently re-sectioned it.
    if (entry.size == sizeof(daw::UiMarkerCommandPayload) &&
        (commandType == daw::UiCommandType::AddMarker ||
         commandType == daw::UiCommandType::RemoveMarker ||
         commandType == daw::UiCommandType::RenameMarker ||
         commandType == daw::UiCommandType::MoveMarker ||
         commandType == daw::UiCommandType::SetMarkerColor)) {
      daw::engine::handleAddMarker(markerCommandDeps, entry, header, commandType);
      return;
    }

    // v29 TIMELINE ops — the meter, and inserting or removing arrangement time.
    if (entry.size == sizeof(daw::UiArrangeTimeCommandPayload) &&
        (commandType == daw::UiCommandType::SetTimeSignature ||
         commandType == daw::UiCommandType::InsertRemoveTime)) {
      daw::UiArrangeTimeCommandPayload tp{};
      std::memcpy(&tp, entry.payload, sizeof(tp));
      if (static_cast<daw::UiCommandType>(tp.commandType) != commandType) {
        return;
      }
      const uint64_t atTick = (static_cast<uint64_t>(tp.nanotickHi) << 32) | tp.nanotickLo;

      // ---- SET TIME SIGNATURE. THIS is where mid-song meter is authored. A Section's meter was
      // reachable from no command at all, which is why that capability was a stub only a
      // hand-edited file could exercise.
      if (commandType == daw::UiCommandType::SetTimeSignature) {
        const daw::TimeSignature sig{tp.numerator, tp.denominator};
        if (!sig.valid()) {
          // REFUSED, not clamped. 4/5 is a typo, not a time signature, and silently turning it
          // into 4/4 would put the ruler somewhere the caller never asked for.
          DAW_EVENT("time_sig.rejected")
              .field("nanotick", atTick)
              .field("numerator", tp.numerator)
              .field("denominator", tp.denominator)
              .field("reason", "invalid_signature");
          historyAppend("set_time_signature", "rejected:invalid_signature", 0xFFFFFFFFu, 0, "");
          return;
        }
        const bool flatten = (tp.flags & daw::kUiTimeSigFlatten) != 0;
        uint32_t pointCount = 0;
        {
          std::lock_guard<std::mutex> alock(arrangeMutex);
          std::vector<daw::TimeSignaturePoint> points;
          if (!flatten) {
            points = songMeter.points();
            points.erase(std::remove_if(points.begin(), points.end(),
                                        [&](const daw::TimeSignaturePoint& p) {
                                          return p.nanotick == atTick;
                                        }),
                         points.end());
          }
          points.push_back({flatten ? 0 : atTick, sig});
          songMeter.setMap(std::move(points));
          pointCount = songMeter.pointCount();
          std::atomic_store_explicit(
              &meterSnapshot,
              std::static_pointer_cast<const daw::TimeSignatureMap>(
                  std::make_shared<daw::TimeSignatureMap>(songMeter)),
              std::memory_order_release);
          // The origin point is also the song-wide pair every older reader uses — the SHM header,
          // the transport payload, the play head's fallback. Kept in step here so the two can
          // never disagree about what bar 1 is in.
          const daw::TimeSignature origin = songMeter.signatureAt(0);
          songTimeSigNum.store(origin.numerator, std::memory_order_relaxed);
          songTimeSigDen.store(origin.denominator, std::memory_order_relaxed);
        }
        arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
        DAW_EVENT("time_sig.set")
            .field("nanotick", atTick)
            .field("numerator", sig.numerator)
            .field("denominator", sig.denominator)
            .field("flatten", flatten)
            .field("points", pointCount);
        historyAppend("set_time_signature", "received", 0xFFFFFFFFu, 0, "");
        return;
      }

      // ---- INSERT / REMOVE TIME: the ripple, as its own command over a tick range.
      //
      // The delta arrives in BARS by default, because a bar is the musical unit and its length
      // depends on the meter in force at that tick — which the engine knows authoritatively and a
      // caller would otherwise re-derive from the published map, with the first disagreement
      // moving the music by the wrong amount.
      int64_t delta = 0;
      {
        std::lock_guard<std::mutex> alock(arrangeMutex);
        if ((tp.flags & daw::kUiTimeEditDeltaIsTicks) != 0) {
          delta = tp.delta;
        } else {
          // THE METER JUST BEFORE THE POINT, not at it. A meter point sitting exactly at `atTick`
          // MOVES with this edit — it is at-or-after — so the bars being inserted are in the
          // PRECEDING meter, not the one that used to start here.
          //
          // Measured: inserting 4 bars at a tick where 7/8 begins used signatureAt(atTick) = 7/8
          // and moved everything by 4 * 3.5 quarters, while the inserted span was still 4/4. The
          // 7/8 point landed at 7.5 bars — off the bar grid — and TimeSignatureMap::setMap then
          // snapped it forward to bar 8, silently parting it from the marker that moved with it.
          const uint64_t probe = atTick > 0 ? atTick - 1 : 0;
          const uint64_t barLen = songMeter.signatureAt(probe).barNanoticks();
          delta = static_cast<int64_t>(tp.delta) * static_cast<int64_t>(barLen);
        }
      }
      if (delta == 0) {
        DAW_EVENT("time_edit.rejected")
            .field("nanotick", atTick)
            .field("reason", "zero_delta");
        historyAppend("insert_remove_time", "rejected:zero_delta", 0xFFFFFFFFu, 0, "");
        return;
      }
      {
        std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> spans;
        const auto trackSnap = snapshotTracks();
        for (auto* rt : trackSnap) {
          if (!rt || rt->removed.load(std::memory_order_acquire)) {
            continue;
          }
          std::lock_guard<std::mutex> tlock(rt->trackMutex);
          for (const auto& pl : rt->sourcePlacements) {
            if (!pl.at.has_value()) {
              continue;
            }
            const uint64_t len = daw::engine::placementLength(pl, rt->ownedClips);
            spans.emplace_back(pl.id, *pl.at, daw::engine::placementReach(*pl.at, len));
          }
        }
        // AUTOMATION IS MATERIAL TOO, and the refusal above guarded only placements.
        //
        // The argument for refusing a shrink into occupied bars is written out on planRipple:
        // rippleTick moves what is at or after the boundary, so material INSIDE the removed
        // bars does not move — the later section boundaries slide over it instead, and a
        // placement that was in the intro is silently now in the verse with no note changed.
        // A filter sweep is re-sectioned by exactly the same mechanism, and there is a second,
        // worse consequence for automation specifically: a point AT the old boundary lands on
        // the new end, and if a point is already there `addPoint` REPLACES it. So the shrink
        // silently destroys one of them, with no undo entry that would put it back.
        //
        // Scanned separately from `spans` rather than folded into planRipple, which stays a
        // pure geometry helper — and reported by track and PARAM, because "something is in the
        // way" is not actionable when the thing is one lane out of sixty.
        if (delta < 0) {
          const uint64_t magnitude = static_cast<uint64_t>(-delta);
          const uint64_t vacatedStart =
              atTick > magnitude ? atTick - magnitude : 0;
          for (auto* rt : trackSnap) {
            if (!rt || rt->removed.load(std::memory_order_acquire)) {
              continue;
            }
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            for (const auto& clip : rt->track.automationClips) {
              for (const auto& pt : clip.points()) {
                if (pt.nanotick >= vacatedStart && pt.nanotick <= atTick) {
                  DAW_EVENT("time_edit.rejected")
                      .field("op", "insert_remove_time")
                      .field("nanotick", atTick)
                      .field("reason", "automation_in_removed_bars")
                      .field("track", rt->trackId)
                      .field("param", clip.paramId())
                      .field("nanotick", pt.nanotick);
                  daw::LogLine() << "UI: InsertRemoveTime refused — automation on track "
                            << rt->trackId << " param '" << clip.paramId()
                            << "' has a point at " << pt.nanotick
                            << ", inside the bars this would remove. Shrinking would leave the "
                               "sweep where it is while the markers slide over it, and would "
                               "collapse a point at the boundary onto the one already there."
                            << std::endl;
                  historyAppend("insert_remove_time",
                                "rejected:automation_in_removed_bars", rt->trackId, 0, "");
                  return;
                }
              }
            }
          }
        }
        const auto plan = daw::planRipple(spans, atTick, delta);
        if (plan.outcome != daw::RippleOutcome::Ok) {
          const bool straddling =
              plan.outcome == daw::RippleOutcome::RefusedStraddlingPlacement;
          const char* reason =
              straddling ? "straddling_placement" : "content_in_removed_bars";
          DAW_EVENT("time_edit.rejected")
              .field("op", "insert_remove_time")
              .field("nanotick", atTick)
              .field("reason", reason)
              .field("blocking_placement", plan.blockingPlacementId);
          if (straddling) {
            daw::LogLine() << "UI: InsertRemoveTime refused — placement "
                      << plan.blockingPlacementId
                      << " crosses the edit point, so the inserted bars would land INSIDE "
                         "it: it would keep its start and length while everything after it "
                         "moved away. Split or shorten it first — whether those bars belong "
                         "inside it or after it is a musical decision this command cannot make."
                      << std::endl;
          } else {
            daw::LogLine() << "UI: InsertRemoveTime refused — placement "
                      << plan.blockingPlacementId
                      << " lives in the bars this would remove. Shrinking would stack it "
                         "onto one tick or delete it; empty those bars first." << std::endl;
          }
          historyAppend("insert_remove_time", (std::string("rejected:") + reason).c_str(),
                        0xFFFFFFFFu, 0, "");
          return;
        }
        // CAPTURED HERE, after every refusal and before the first mutation, so a refused ripple
        // costs nothing and an applied one is fully recoverable.
        SongStoreState songBefore = snapshotSongStore();
        // APPLY. There is no spine to update — the TIMELINE is what moves, and everything keyed
        // to a tick moves with it.
        uint32_t markersMoved = 0;
        uint32_t meterMoved = 0;
        {
          std::lock_guard<std::mutex> alock(arrangeMutex);
          markersMoved = markerList.rippleFrom(atTick, delta);
          // THE METER MOVES TOO, and this is the thing the spine could never reach: a 7/8 bridge
          // is a point in this map, so inserting bars before it has to carry it or the bridge
          // lands in the wrong place. The spine could not have this bug — its meter was welded to
          // a section — and could not have the capability either, since no command could set one.
          auto meterPoints = songMeter.points();
          for (auto& pt : meterPoints) {
            // Never the origin at 0: a map with no point at tick 0 has no meter before its first
            // change, which is the same rule the tempo map follows two blocks down.
            if (pt.nanotick != 0 && pt.nanotick >= atTick) {
              pt.nanotick = daw::rippleTick(pt.nanotick, atTick, delta);
              ++meterMoved;
            }
          }
          if (meterMoved > 0) {
            songMeter.setMap(std::move(meterPoints));
            std::atomic_store_explicit(
                &meterSnapshot,
                std::static_pointer_cast<const daw::TimeSignatureMap>(
                    std::make_shared<daw::TimeSignatureMap>(songMeter)),
                std::memory_order_release);
          }
        }
        // AND THE SONG-LEVEL TIMELINES. The ripple moved every placement and every automation
        // point, and left a tempo change and a key change sitting at their absolute ticks — so
        // inserting bars into the intro slid the material later and left the tempo change and
        // the modulation firing in the middle of what used to follow them. The comment on the
        // automation ripple makes exactly this argument; it simply was not applied here.
        //
        // THE METER NEEDS NO RIPPLE AT ALL. It used to be the open question here — a
        // tick-keyed map meant a section's length was computed THROUGH the meter, so moving
        // meter points changed the very delta derived from them, and whether a meter change
        // belonged to the section or to the timeline decided the answer. The meter now lives ON
        // the section, so a section carries its meter with it by construction and there is
        // nothing to move. The question is not answered, it is dissolved.
        uint32_t tempoMoved = 0;
        for (auto& pt : loadedTempoMap) {
          // Never the anchor at 0: a tempo map without a point at the origin has no tempo
          // before its first change.
          if (pt.nanotick != 0 && pt.nanotick >= atTick) {
            pt.nanotick = daw::rippleTick(pt.nanotick, atTick, delta);
            ++tempoMoved;
          }
        }
        if (tempoMoved > 0) {
          std::sort(loadedTempoMap.begin(), loadedTempoMap.end(),
                    [](const daw::ProjectTempoPoint& a, const daw::ProjectTempoPoint& b) {
                      return a.nanotick < b.nanotick;
                    });
          // The provider is what the transport actually reads, so a retained map that moved
          // and a provider that did not would play at the old tempo positions and save at the
          // new ones — the same divergence the automation republish above exists to prevent.
          std::vector<daw::TempoPoint> pts;
          pts.reserve(loadedTempoMap.size());
          for (const auto& pt : loadedTempoMap) {
            pts.push_back({pt.nanotick, pt.bpm});
          }
          tempoProvider.setMap(std::move(pts));
        }
        uint32_t harmonyMoved = 0;
        {
          std::lock_guard<std::mutex> hlock(harmonyMutex);
          for (auto& ev : harmonyEvents) {
            if (ev.nanotick >= atTick) {
              ev.nanotick = daw::rippleTick(ev.nanotick, atTick, delta);
              ++harmonyMoved;
            }
          }
          if (harmonyMoved > 0) {
            std::sort(harmonyEvents.begin(), harmonyEvents.end(),
                      [](const daw::HarmonyEvent& a, const daw::HarmonyEvent& b) {
                        return a.nanotick < b.nanotick;
                      });
          }
        }
        if (harmonyMoved > 0) {
          harmonyDirty.store(true, std::memory_order_release);
          harmonyVersion.fetch_add(1, std::memory_order_acq_rel);
        }
        for (auto* rt : trackSnap) {
          if (!rt || rt->removed.load(std::memory_order_acquire)) {
            continue;
          }
          std::shared_ptr<const ClipSnapshot> snap;
          std::shared_ptr<const TrackStateSnapshot> stateSnap;
          {
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            bool touched = false;
            for (auto& pl : rt->sourcePlacements) {
              if (!pl.at.has_value()) {
                continue;
              }
              const uint64_t moved = daw::rippleTick(*pl.at, atTick, delta);
              if (moved != *pl.at) {
                pl.at = moved;
                touched = true;
              }
            }
            // M3.27: automation moves WITH the material. Without this, inserting bars
            // into the intro slid every note later and left the filter sweep where it
            // was — the notes and the automation would drift apart by exactly the amount
            // of the edit, silently.
            for (auto& clip : rt->track.automationClips) {
              // Only points at or after the boundary move, matching rippleTick's rule for
              // placements, so a sweep earlier in the song stays put.
              //
              // This used to rebuild the clip inline — construct a fresh one, re-addPoint every
              // point through rippleTick — because AutomationClip's own helper shifted EVERY
              // point and so could not be used. The helper has the right rule now
              // (AutomationClip::rippleFrom) and the duplication is gone. The rebuild also had a
              // hazard the direct move does not: addPoint REPLACES at a colliding tick, so a
              // negative delta that collapsed two points destroyed one. The caller refuses that
              // case up front, but relying on a refusal to prevent silent data loss two layers
              // down is thinner than not having the hazard.
              if (clip.rippleFrom(atTick, delta)) {
                touched = true;
              }
            }
            if (!touched) {
              continue;
            }
            snap = rebuildFlatAndPublish(*rt);
            std::atomic_store_explicit(&rt->audioRender, rebuildAudioRender(*rt),
                                       std::memory_order_release);
            // And the TRACK snapshot, which is the only copy of the automation the RT
            // scheduler ever reads. Without this the ripple moved the points in the model
            // and in the saved file while what PLAYED stayed at the old positions — so the
            // sweep was in the right place on disk, the wrong place in your ears, and it
            // jumped the next time the project was opened. WriteAutomationPoint already
            // says exactly this ("a point that is not republished is a point that does not
            // play"); the rule just was not applied here. automation_check missed it by
            // reading only the saved file.
            stateSnap = buildTrackSnapshot(rt->track);
          }
          if (snap) {
            std::atomic_store_explicit(&rt->clipSnapshot, snap,
                                       std::memory_order_release);
          }
          if (stateSnap) {
            std::atomic_store_explicit(&rt->trackSnapshot, stateSnap,
                                       std::memory_order_release);
          }
          bumpClipVersionFor(rt);
        }
        clipDirty.store(true, std::memory_order_release);
        // The ripple rebuilds automation clips, so anything caching lanes has to re-read. Bumped
        // unconditionally rather than only when a point moved: the cost of one extra re-read is a
        // re-read, and the cost of missing one is a curve drawn in the wrong place.
        automationVersion.fetch_add(1, std::memory_order_acq_rel);
        recomputeSongEnd();
        arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
        {
          EngineUndoEntry e;
          e.song = true;
          e.songBefore = std::move(songBefore);
          e.songAfter = snapshotSongStore();
          pushUndo(std::move(e));
        }
        DAW_EVENT("time.edited")
            .field("nanotick", atTick)
            .field("delta_ticks", static_cast<int64_t>(delta))
            .field("placements_moved", plan.moved)
            .field("tempo_points_moved", tempoMoved)
            .field("harmony_events_moved", harmonyMoved)
            .field("markers_moved", markersMoved)
            .field("meter_points_moved", meterMoved)
            .field("undoable", true);
        // JOURNALLED ON SUCCESS. Only the rejections were recorded, so history.jsonl held every
        // refused ripple and no applied one — the opposite of what a "what changed since Tuesday"
        // artifact is for.
        historyAppend("insert_remove_time", "received", 0xFFFFFFFFu, 0, "");
        return;      }
      return;
    }
    if (entry.size == sizeof(daw::UiTrackRoutingPayload) &&
        commandType == daw::UiCommandType::SetTrackRouting) {
      daw::engine::handleSetTrackRouting(trackCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiModLinkCommandPayload) &&
        (commandType == daw::UiCommandType::AddModLink ||
         commandType == daw::UiCommandType::RemoveModLink ||
         commandType == daw::UiCommandType::SetModLinkDepth)) {
      daw::engine::handleAddModLink(modlinkCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiModLinkUid16Payload) &&
        commandType == daw::UiCommandType::SetModLinkUid16) {
      daw::engine::handleSetModLinkUid16(modlinkCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiModSourceValuePayload) &&
        commandType == daw::UiCommandType::SetModSourceValue) {
      daw::engine::handleSetModSourceValue(modlinkCommandDeps, entry, header, commandType);
      return;
    }
    // PER-DEVICE PATCHER EDITS. "Patcher is a device" moved the DATA model and the read-back to
    // per-device graphs; the EDIT commands were never migrated and still addressed the one shared
    // pool. For any project carrying per-device graphs that meant an edit landed in the pool and
    // was never saved — applied, reported as applied, and gone on reload. Before the save guard it
    // was worse: the same edit overwrote device 1's real graph with the whole pool.
    //
    // A SEPARATE BRANCH rather than a rewrite of the one below. The legacy whole-pool path is
    // untouched, so a caller that does not ask for a device cannot be broken by this, and the new
    // path is self-contained enough to read in one sitting.
    //
    // The edit is applied through the SAME helpers by wrapping the device's graph in a scratch
    // PatcherGraphState. Reimplementing the cycle and port validation for device graphs is exactly
    // how the two paths would drift into disagreeing about which edits are legal.
    if (entry.size == sizeof(daw::UiPatcherGraphCommandPayload) &&
        (commandType == daw::UiCommandType::AddPatcherNode ||
         commandType == daw::UiCommandType::RemovePatcherNode ||
         commandType == daw::UiCommandType::ConnectPatcherNodes)) {
      daw::engine::handleAddPatcherNode(patcherCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherNodeConfigPayload) &&
        commandType == daw::UiCommandType::SetPatcherNodeConfig) {
      daw::engine::handleSetPatcherNodeConfig(patcherCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        commandType == daw::UiCommandType::SetTrackName) {
      daw::engine::handleSetTrackName(trackCommandDeps, entry, header, commandType);
      return;
    }
    // ---- SAMPLER EMIT ROWS (78). Writes the pattern that reproduces the chop.
    if (entry.size == sizeof(daw::UiSamplerEmitRowsPayload) &&
        commandType == daw::UiCommandType::SamplerEmitRows) {
      daw::engine::handleSamplerEmitRows(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SET ROW OPS (81).
    //
    // Checked against the opcode as well as the size, like every other handler here: three
    // command payloads are 40 bytes and dispatching on size alone would route them to whichever
    // branch happened to be tested first.
    if (entry.size == sizeof(daw::UiSetRowOpsPayload) &&
        commandType == daw::UiCommandType::SetRowOps) {
      daw::engine::handleSetRowOps(rowopsCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SLICE (76) and SAMPLER MARKER (77).
    //
    // Both edit the SliceSet and then refresh the snapshot, so a re-chop takes effect on the NEXT
    // note without touching a single row. That is §5.1: the extent is derived from marker order,
    // so nothing downstream had to be rewritten.
    if ((entry.size == sizeof(daw::UiSamplerSlicePayload) &&
         commandType == daw::UiCommandType::SamplerSlice) ||
        (entry.size == sizeof(daw::UiSamplerMarkerPayload) &&
         commandType == daw::UiCommandType::SamplerMarker)) {
      daw::engine::handleSamplerSlice(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- REQUEST SAMPLER ENVELOPE (97). One modulator's SHAPE into a seqlock slot.
    //
    // SamplerSetEnvelopePoints (84) could write a full multi-segment envelope and nothing could
    // read one back, so a pencil editor built on it would be write-only — able to send a curve
    // and never to draw the one already in the project. The kit read-back's modMask says WHICH
    // targets are configured and cannot say what shape.
    if (entry.size == sizeof(daw::UiSamplerEnvelopeRequestPayload) &&
        commandType == daw::UiCommandType::RequestSamplerEnvelope) {
      daw::engine::handleRequestSamplerEnvelope(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- REQUEST SAMPLER KIT (75). Publishes one device's kit into a seqlock slot.
    if (entry.size == sizeof(daw::UiSamplerKitRequestPayload) &&
        commandType == daw::UiCommandType::RequestSamplerKit) {
      daw::engine::handleRequestSamplerKit(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SET SLOT (74). One field of one slot.
    if (entry.size == sizeof(daw::UiSamplerSetSlotPayload) &&
        commandType == daw::UiCommandType::SamplerSetSlot) {
      daw::engine::handleSamplerSetSlot(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SET DEVICE (88). The three device-level fields, none of which had a command.
    //
    // `defaultGate` is the one that was asked for (owner: "could that be a setting per bank?
    // 'ignore note-offs'"). `voiceCap` and `defaultView` were already persisted and already
    // rendered and reachable by nothing — the same "the engine reads a field it has no path to
    // write" shape this suite keeps finding, sitting one field id away from the thing being
    // added, so they are closed here rather than left for a later opcode.
    if (entry.size == sizeof(daw::UiSamplerSetDevicePayload) &&
        commandType == daw::UiCommandType::SamplerSetDevice) {
      daw::engine::handleSamplerSetDevice(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SET FILTER (86). The field nothing could write.
    //
    // Before this, modSet.filterType was read at the kit publish site and written NOWHERE — the
    // only way to turn a sampler's filter on was to hand-edit the project JSON. So every cutoff
    // and resonance modulator reachable from the CLI or the UI modulated a filter that was off:
    // the modulator existed, saved, reloaded and published its bit, and moved nothing.
    //
    // Note this is a MOD SET property and not a slot property. Slots share mod sets, so turning
    // the filter on is one edit for every slot that points at it, which is the behaviour a kit
    // wants — a chop's twenty slices are one instrument, not twenty.
    if (entry.size == sizeof(daw::UiSamplerFilterPayload) &&
        commandType == daw::UiCommandType::SamplerSetFilter) {
      daw::engine::handleSamplerSetFilter(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SET CLIP GRID (94): a clip's OWN subdivision and meter. Task #43 phase 2.
    //
    // ProjectClip has carried linesPerBeat + a time signature since the grid moved off the track;
    // all three persist, all three publish packed into UiClipExtent's flag bits, and the tracker
    // draws from the CLIP's grid before the track's. So the authority in that chain was the one
    // thing no command could write — a verse in 4 against a bridge in 3 was reachable only by
    // hand-editing the project file.
    if (entry.size == sizeof(daw::UiSetClipGridPayload) &&
        commandType == daw::UiCommandType::SetClipGrid) {
      daw::engine::handleSetClipGrid(clipCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SET AUDIO CLIP FIELD (95): an audio region's in-point, gain and fades.
    //
    // All four persist, all four publish, and the renderer bakes all four into the region it
    // schedules — and until this, no command wrote any of them. An audio clip was READ-ONLY from
    // every surface: the UI could draw a clip gain and a fade handle and could not move them, and
    // the only way to change one was a text editor on the project file. Found by giving
    // persisted_field_reach a CLIP scope, which it had never had.
    if (entry.size == sizeof(daw::UiAudioClipFieldPayload) &&
        commandType == daw::UiCommandType::SetAudioClipField) {
      daw::engine::handleSetAudioClipField(clipCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SET VINTAGE (91): bit depth and rate reduction, the SP-1200 character. A mod
    // set property for the same reason the filter is one — a chop's twenty slices are one
    // instrument.
    if (entry.size == sizeof(daw::UiSamplerVintagePayload) &&
        commandType == daw::UiCommandType::SamplerSetVintage) {
      daw::engine::handleSamplerSetVintage(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SET LFO (85). The modulator kind that saved, loaded and made no sound.
    if (entry.size == sizeof(daw::UiSamplerLfoPayload) &&
        commandType == daw::UiCommandType::SamplerSetLfo) {
      daw::engine::handleSamplerSetLfo(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER SET ENVELOPE (82). The ADSR, which nothing could reach before.
    if (entry.size == sizeof(daw::UiSamplerEnvelopePayload) &&
        commandType == daw::UiCommandType::SamplerSetEnvelope) {
      daw::engine::handleSamplerSetEnvelope(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAMPLER LOAD (73). Mints a SOURCE and a SLOT that plays it.
    if (entry.size == sizeof(daw::UiSamplerLoadPayload) &&
        commandType == daw::UiCommandType::SamplerLoad) {
      daw::engine::handleSamplerLoad(samplerCommandDeps, entry, header, commandType);
      return;
    }

    // ---- SAVE/LOAD MODULE (79/80). The .uni: one file you can send someone.
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        (commandType == daw::UiCommandType::SaveModule ||
         commandType == daw::UiCommandType::LoadModule)) {
      daw::engine::handleSaveModule(moduleCommandDeps, entry, header, commandType);
      return;
    }

    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        (commandType == daw::UiCommandType::SaveProject ||
         commandType == daw::UiCommandType::LoadProject)) {
      daw::engine::handleSaveProject(projectCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        commandType == daw::UiCommandType::SavePatcherPreset) {
      daw::engine::handleSavePatcherPreset(patcherCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiDeviceEuclideanConfigPayload) &&
        commandType == daw::UiCommandType::SetDeviceEuclideanConfig) {
      daw::engine::handleSetDeviceEuclideanConfig(trackCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size == sizeof(daw::UiChainCommandPayload) &&
        (commandType == daw::UiCommandType::AddDevice ||
         commandType == daw::UiCommandType::RemoveDevice ||
         commandType == daw::UiCommandType::MoveDevice ||
         commandType == daw::UiCommandType::UpdateDevice)) {
      daw::engine::handleAddDevice(chainCommandDeps, entry, header, commandType);
      return;
    }
    if (entry.size != sizeof(daw::UiCommandPayload)) {
      daw::LogLine() << "UI: bad UiCommand size " << entry.size
                << " (expected " << sizeof(daw::UiCommandPayload) << ")"
                << std::endl;
      return;
    }
    daw::UiCommandPayload payload{};
    std::memcpy(&payload, entry.payload, sizeof(payload));
    if (payload.commandType ==
        static_cast<uint16_t>(daw::UiCommandType::LoadPluginOnTrack)) {
      daw::engine::handleLoadPluginOnTrack(deviceCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::OpenPluginEditor)) {
      daw::engine::handleOpenPluginEditor(deviceCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::WriteNote)) {
      daw::engine::handleWriteNote(noteCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::DeleteNote)) {
      daw::engine::handleDeleteNote(noteCommandDeps, entry, payload);
    } else if (payload.commandType ==
                   static_cast<uint16_t>(daw::UiCommandType::ForkPlacementClip) ||
               payload.commandType ==
                   static_cast<uint16_t>(daw::UiCommandType::SwapPlacementClip) ||
               payload.commandType ==
                   static_cast<uint16_t>(daw::UiCommandType::ClearPlacementAlternate)) {
      // M2.57 SCRATCH CLIPS. value0 = placementId.
      //
      // The problem: an agent that writes into your clip leaves you undoing its work, with its
      // edits interleaved with yours in one undo stack and no way to hear the two side by side.
      // The model already had the right primitive — a clip is CONTENT and a placement is an
      // APPEARANCE — so "the agent's version" is just another clip, and comparing is retargeting
      // the appearance.
      //
      // WHAT PLAYS IS ALWAYS clipId. There is deliberately no "auditioning" flag: a second fact
      // about which clip you are hearing is a second fact that can disagree with the first, and
      // this codebase has spent most of its debugging time on exactly that shape.
      const auto scratchOp = static_cast<daw::UiCommandType>(payload.commandType);
      const uint32_t placementId = payload.value0;
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
      if (!runtime) {
        DAW_EVENT("scratch.rejected")
            .field("op", daw::uiCommandTypeName(scratchOp))
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_track");
        return;
      }
      bool found = false;
      const char* reason = "no_such_placement";
      uint32_t nowPlaying = 0;
      uint32_t alternate = 0;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& pl : runtime->sourcePlacements) {
          if (pl.id != placementId) {
            continue;
          }
          if (scratchOp == daw::UiCommandType::ForkPlacementClip) {
            // COPY the clip this placement plays, point the placement at the copy, and keep the
            // original as the alternate. Only THIS placement is retargeted — other appearances of
            // the same clip keep playing the original, which is the whole point of forking rather
            // than editing: "fix the bass in chorus 1" still reaches all three choruses, and a
            // draft of chorus 1 does not.
            const daw::ProjectClip* source = nullptr;
            for (const auto& c : runtime->ownedClips) {
              if (c.id == pl.clipId) {
                source = &c;
                break;
              }
            }
            if (!source) {
              reason = "no_such_clip";
              break;
            }
            daw::ProjectClip copy = *source;
            copy.id = nextClipId.fetch_add(1, std::memory_order_acq_rel);
            copy.name = source->name + " (draft)";
            runtime->ownedClips.push_back(std::move(copy));
            runtime->editableClipIds.push_back(runtime->ownedClips.back().id);
            pl.alternateClipId = pl.clipId;
            pl.clipId = runtime->ownedClips.back().id;
            found = true;
          } else if (scratchOp == daw::UiCommandType::SwapPlacementClip) {
            if (pl.alternateClipId == 0) {
              reason = "no_alternate";
              break;
            }
            std::swap(pl.clipId, pl.alternateClipId);
            found = true;
          } else {
            if (pl.alternateClipId == 0) {
              reason = "no_alternate";
              break;
            }
            pl.alternateClipId = 0;
            found = true;
          }
          nowPlaying = pl.clipId;
          alternate = pl.alternateClipId;
          break;
        }
        if (found) {
          // RE-DERIVE rather than bump. The published extents are built inside
          // rebuildFlatAndPublish, so bumping the version alone rebuilds the region from a stale
          // vector and the swap is inaudible AND invisible — the exact failure the edit-scope
          // toggle hit.
          snapshot = rebuildFlatAndPublish(*runtime);
          std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                     std::memory_order_release);
        }
      }
      if (!found) {
        DAW_EVENT("scratch.rejected")
            .field("op", daw::uiCommandTypeName(scratchOp))
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", reason);
        return;
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      bumpClipVersionFor(runtime);
      clipDirty.store(true, std::memory_order_release);
      recomputeSongEnd();
      DAW_EVENT("scratch.applied")
          .field("op", daw::uiCommandTypeName(scratchOp))
          .field("track", payload.trackId)
          .field("placement", placementId)
          .field("playing_clip", nowPlaying)
          .field("alternate_clip", alternate);
      historyAppend(daw::uiCommandTypeName(scratchOp), "received", payload.trackId, 0, "");
      return;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetPlacementEditScope)) {
      // value0 = placementId, flags bit0 = on. Deliberately NOT version-gated: this changes no
      // note, so it cannot invalidate anyone's in-flight edit — the same reasoning that keeps a
      // section rename off the clip version.
      const uint32_t placementId = payload.value0;
      const bool on = (payload.flags & 1u) != 0;
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
      if (!runtime) {
        DAW_EVENT("placement_scope.rejected")
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_track");
        return;
      }
      bool found = false;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& pl : runtime->sourcePlacements) {
          if (pl.id != placementId) {
            continue;
          }
          found = true;
          pl.localEdits = on;
          break;
        }
        if (found) {
          // RE-DERIVE, don't just bump. The published extents are rebuilt from
          // rt.clipExtents, and clipExtents is DERIVED inside rebuildFlatAndPublish — so
          // bumping the clip version alone rebuilt the region out of a stale vector and the
          // flag stayed false. The read-back existed and reported the old answer, which is
          // worse than not having it: a UI would have drawn the toggle as off after setting it.
          snapshot = rebuildFlatAndPublish(*runtime);
        }
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      if (!found) {
        // Naming a placement that is not there can never succeed on a retry, so say so rather
        // than reporting a scope change that did not happen.
        DAW_EVENT("placement_scope.rejected")
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_placement");
        return;
      }
      // The published extents carry the flag, and they rebuild on the clip version — so bump it
      // or the toggle stays invisible until some unrelated note edit happens to republish.
      bumpClipVersionFor(runtime);
      clipDirty.store(true, std::memory_order_release);
      DAW_EVENT("placement_scope.set")
          .field("track", payload.trackId)
          .field("placement", placementId)
          .field("local", on);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RevertPlacementOverrides)) {
      // M3.24: the one-click revert. Clears BOTH override vectors on one placement, which
      // is only this simple because the overrides are additive-only — there are no
      // inverses to replay, just two lists to drop.
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::RevertPlacementOverrides,
                                      payload.trackId)) {
        return;
      }
      const uint32_t placementId = payload.value0;
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
      if (!runtime) {
        DAW_EVENT("overrides.revert_rejected")
            .field("track", payload.trackId)
            .field("reason", "no_such_track");
        return;
      }
      uint32_t clearedAdds = 0, clearedMutes = 0;
      bool found = false;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        // "No inverses to replay, just two lists to drop" is true of the FORWARD op and
        // was the wrong conclusion about undo: this is the most destructive edit in the
        // whole override feature — it throws away every add and mute on an appearance at
        // once — and it pushed nothing. With undo being a whole-store swap, the next Ctrl-Z
        // both failed to restore what revert deleted AND rolled back some older edit
        // instead. The store snapshot carries the placements, so recording it is enough.
        TrackStoreState storeBefore = snapshotTrackStore(*runtime);
        for (auto& pl : runtime->sourcePlacements) {
          if (pl.id != placementId) {
            continue;
          }
          found = true;
          clearedAdds = static_cast<uint32_t>(pl.adds.size());
          clearedMutes = static_cast<uint32_t>(pl.mutes.size());
          pl.adds.clear();
          pl.mutes.clear();
          break;
        }
        if (found && (clearedAdds > 0 || clearedMutes > 0)) {
          runtime->arrangementDirty.store(true, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          pushStructuralUndo(payload.trackId, std::move(storeBefore),
                             snapshotTrackStore(*runtime));
        }
      }
      if (!found) {
        DAW_EVENT("overrides.revert_rejected")
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_placement");
        return;
      }
      if (clearedAdds == 0 && clearedMutes == 0) {
        // Nothing to revert is not a failure, but it is worth saying: a UI that offered
        // the button on a placement with no overrides is showing an action that does
        // nothing.
        DAW_EVENT("overrides.revert_noop").field("placement", placementId);
        return;
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      bumpClipVersionFor(runtime);
      clipDirty.store(true, std::memory_order_release);
      DAW_EVENT("overrides.reverted")
          .field("track", payload.trackId)
          .field("placement", placementId)
          .field("adds_cleared", clearedAdds)
          .field("mutes_cleared", clearedMutes);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::PreviewNote)) {
      // Keyjazz: audition a pitch on the track's instrument without touching the clip
      // store. Enqueue for the producer to inject into the track's event ring. Velocity 0
      // on an on-gesture is a note-off (running-status convention) so a key can't stick.
      const uint8_t pitch =
          static_cast<uint8_t>(std::min<uint32_t>(payload.notePitch, 127));
      const uint8_t velocity =
          static_cast<uint8_t>(std::min<uint32_t>(payload.value0, 127));
      const bool on =
          (payload.flags & daw::kPreviewNoteFlagOn) != 0 && velocity > 0;
      enqueuePreview(payload.trackId, pitch, velocity, on);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::AddTrack)) {
      // Add an empty top-level track. Refill the LOWEST tombstone first (RemoveTrack leaves
      // middle holes) so repeated middle remove+add can't leak slots toward the cap; only
      // when there is no tombstone do we append at the extent. Its id == slot index and is
      // stable. A reused slot gets a bare host + blank state; a fresh extent slot is created.
      uint32_t slot = liveTrackCount.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (uint32_t i = 0; i < slot && i < tracks.size(); ++i) {
          if (tracks[i] && tracks[i]->removed.load(std::memory_order_acquire)) {
            slot = i;  // lowest tombstone — refill it instead of appending
            break;
          }
        }
      }
      if (slot >= daw::kUiMaxTracks) {
        daw::LogLine() << "UI: AddTrack refused — at track cap " << daw::kUiMaxTracks
                  << std::endl;
      } else {
        TrackRuntime* existing = daw::engine::trackAt(tracks, tracksMutex, slot);
        bool ok = true;
        if (existing) {
          ok = restartTrackHost(*existing, {});
          if (ok) {
            {
              std::lock_guard<std::mutex> tlock(existing->trackMutex);
              resetTrackContent(*existing);
              existing->trackName = "Track " + std::to_string(slot + 1);
              existing->trackSnapshot = buildTrackSnapshot(existing->track);
            }
            existing->isAuxChild.store(false, std::memory_order_release);
            existing->parentId.store(0, std::memory_order_relaxed);
            existing->collapsed.store(false, std::memory_order_relaxed);
            existing->childrenReconciled.store(false, std::memory_order_relaxed);
            existing->removed.store(false, std::memory_order_release);
            auto snapshot = rebuildFlatAndPublish(*existing);
            if (snapshot) {
              std::atomic_store_explicit(&existing->clipSnapshot, snapshot,
                                         std::memory_order_release);
            }
          }
        } else {
          auto rt = setupTrackRuntime(slot, "", false, true);
          if (!rt) {
            ok = false;
          } else {
            std::lock_guard<std::mutex> lock(tracksMutex);
            tracks.push_back(std::move(rt));
          }
        }
        if (ok) {
          uint32_t seen = liveTrackCount.load(std::memory_order_relaxed);
          while (slot + 1 > seen &&
                 !liveTrackCount.compare_exchange_weak(seen, slot + 1,
                                                       std::memory_order_relaxed)) {
          }
          {
            // A fresh track's clips are empty, but the RuntimeTrack in this slot may be
            // a reused tombstone whose counter still carries the removed track's value.
            // Bump so nobody's pre-existing base is accepted against a brand-new track,
            // and so the version-gated regions rebuild and show the new lane.
            std::lock_guard<std::mutex> lock(tracksMutex);
            if (slot < tracks.size() && tracks[slot]) {
              tracks[slot]->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
            }
          }
          clipVersion.fetch_add(1, std::memory_order_acq_rel);
          std::cout << "UI: AddTrack -> track " << slot << std::endl;
        } else {
          daw::LogLine() << "UI: AddTrack failed to bring up track " << slot << std::endl;
        }
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RemoveTrack)) {
      // Tombstone the target track (stable id == slot) + its aux children. The slot is
      // kept (kUiTrackFlagAbsent) so neighbours keep their ids; trailing tombstones are
      // trimmed so removing from the end shrinks the extent. Rejects a child id.
      const uint32_t targetId = payload.trackId;
      std::vector<TrackRuntime*> toRemove;
      bool rejected = false;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& rt : tracks) {
          if (!rt) {
            continue;
          }
          const bool isChild = rt->isAuxChild.load(std::memory_order_acquire);
          if (rt->trackId == targetId) {
            if (isChild) {
              rejected = true;
              break;
            }
            toRemove.push_back(rt.get());
          } else if (isChild &&
                     rt->auxParentTrackId.load(std::memory_order_relaxed) == targetId) {
            toRemove.push_back(rt.get());
          }
        }
      }
      if (rejected) {
        daw::LogLine() << "UI: RemoveTrack rejected — track " << targetId
                  << " is an aux child (managed via its parent's buses)" << std::endl;
      } else if (toRemove.empty()) {
        daw::LogLine() << "UI: RemoveTrack — no track with id " << targetId << std::endl;
      } else {
        for (TrackRuntime* rt : toRemove) {
          // Tear the host down and blank the track, mirroring the load-clear sequence, then
          // mark it a tombstone. Runs on the command thread with no tracksMutex held, so
          // taking controllerMutex is safe.
          {
            std::lock_guard<std::mutex> clock(rt->controllerMutex);
            rt->needsRestart.store(false, std::memory_order_release);
            rt->hostReady.store(false, std::memory_order_release);
            rt->active.store(false, std::memory_order_release);
            rt->hostGaveUp.store(false, std::memory_order_release);
            rt->watchdog.reset();
            rt->controller.disconnect();
            rt->config.pluginPaths.clear();
            rt->config.pluginNames.clear();
            rt->lastAuxOutMask.store(0, std::memory_order_relaxed);
            rt->lastSidechainMask.store(0, std::memory_order_relaxed);
          }
          std::shared_ptr<const ClipSnapshot> snapshot;
          {
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            rt->track.chain = daw::TrackChain{};
            rt->sourcePlacements.clear();
            rt->ownedClips.clear();
            rt->editableClipIds.clear();
            rt->arrangementDirty.store(false, std::memory_order_relaxed);
            // Republish the (now empty) flat clip + audio render, exactly like the
            // load-clear does. Without this the removed track's notes linger in the
            // published flat clip until reload — the schedule already drops them (its host
            // is gone and its clips are cleared), but the UI aggregate keeps showing them.
            snapshot = rebuildFlatAndPublish(*rt);
            std::atomic_store_explicit(&rt->audioRender, rebuildAudioRender(*rt),
                                       std::memory_order_release);
          }
          if (snapshot) {
            std::atomic_store_explicit(&rt->clipSnapshot, snapshot,
                                       std::memory_order_release);
          }
          rt->isAuxChild.store(false, std::memory_order_release);
          rt->parentId.store(0, std::memory_order_relaxed);
          rt->childrenReconciled.store(false, std::memory_order_relaxed);
          rt->removed.store(true, std::memory_order_release);
          // This wiped every clip on the track, which is as big a clip change as there
          // is — so both counters have to move. Without the GLOBAL bump the
          // version-gated regions are never rebuilt and the removed track's notes stay
          // published; without the PER-TRACK bump, a base read before the removal is
          // still accepted against the now-empty track, and because AddTrack reuses this
          // same TrackRuntime, that stale base carries over to the NEW track in this slot.
          bumpClipVersionFor(rt);
        }
        // Trim trailing tombstones so a remove-from-the-end shrinks the extent (and the
        // freed slot is reused by the next AddTrack).
        std::lock_guard<std::mutex> lock(tracksMutex);
        uint32_t extent = liveTrackCount.load(std::memory_order_relaxed);
        while (extent > 0) {
          const uint32_t last = extent - 1;
          if (last < tracks.size() && tracks[last] &&
              tracks[last]->removed.load(std::memory_order_acquire)) {
            extent = last;
          } else {
            break;
          }
        }
        liveTrackCount.store(extent, std::memory_order_release);
        std::cout << "UI: RemoveTrack " << targetId << " (+"
                  << (toRemove.size() - 1) << " children), extent now " << extent
                  << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::MovePlacement)) {
      // Move a placement to a new `at` (arrangement drag). value0 = stable placementId,
      // noteNanotick = new at, notePitch = new trackId (0xFFFFFFFF = same track). Cross-
      // track lane drags are a v2 (the clip would have to move ownership); same-track now.
      const uint32_t placementId = payload.value0;
      const uint64_t newAt = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                             payload.noteNanotickLo;
      const uint32_t newTrackId = payload.notePitch;
      if (newTrackId != 0xFFFFFFFFu && newTrackId != payload.trackId) {
        // Cross-track lane drag: relocate the placement + its clip to another lane, both
        // tracks committed atomically under one undo entry (no state where the clip belongs
        // to neither). Clip ids are globally unique, so the dest just needs its own copy of
        // the referenced clip for the flatten to resolve it.
        const uint32_t srcId = payload.trackId;
        const uint32_t dstId = newTrackId;
        TrackRuntime* src = nullptr;
        // NOT trackAt: this resolves BOTH tracks under ONE lock. Two trackAt calls would take
        // the mutex twice and lose atomicity across the pair, so a concurrent add between them
        // could hand back a src and dst from different states of the table.
        TrackRuntime* dst = nullptr;
        {
          std::lock_guard<std::mutex> lock(tracksMutex);
          if (srcId < tracks.size()) src = tracks[srcId].get();
          if (dstId < tracks.size()) dst = tracks[dstId].get();
        }
        bool ok = false;
        if (src && dst && src != dst) {
          std::scoped_lock lock(src->trackMutex, dst->trackMutex);
          TrackStoreState srcBefore = snapshotTrackStore(*src);
          TrackStoreState dstBefore = snapshotTrackStore(*dst);
          auto it = std::find_if(
              src->sourcePlacements.begin(), src->sourcePlacements.end(),
              [&](const daw::ProjectPlacement& p) { return p.id == placementId; });
          // Give the dest its OWN copy of the referenced clip under a FRESH globally-unique
          // id, and repoint the moved placement to it. Reusing the source id would put the
          // same id in two tracks; if that clip is referenced elsewhere, a later in-place
          // edit (forkOwnedClip skips the copy-on-write when the id is already editable)
          // diverges under the shared id, and save's dedup-by-id silently drops one copy.
          daw::ProjectClip dstClip;
          bool clipCopied = false;
          if (it != src->sourcePlacements.end()) {
            for (const auto& c : src->ownedClips) {
              if (c.id == it->clipId) {
                dstClip = c;
                clipCopied = true;
                break;
              }
            }
          }
          if (it != src->sourcePlacements.end() && clipCopied) {
            daw::ProjectPlacement moved = *it;
            moved.at = newAt;
            dstClip.id = nextClipId.fetch_add(1, std::memory_order_acq_rel);
            moved.clipId = dstClip.id;
            dst->ownedClips.push_back(std::move(dstClip));
            dst->editableClipIds.push_back(moved.clipId);
            src->sourcePlacements.erase(it);
            dst->sourcePlacements.push_back(std::move(moved));
            src->arrangementDirty.store(true, std::memory_order_relaxed);
            dst->arrangementDirty.store(true, std::memory_order_relaxed);
            auto srcSnap = rebuildFlatAndPublish(*src);
            auto dstSnap = rebuildFlatAndPublish(*dst);
            std::atomic_store_explicit(&src->audioRender, rebuildAudioRender(*src),
                                       std::memory_order_release);
            std::atomic_store_explicit(&dst->audioRender, rebuildAudioRender(*dst),
                                       std::memory_order_release);
            if (srcSnap) {
              std::atomic_store_explicit(&src->clipSnapshot, srcSnap,
                                         std::memory_order_release);
            }
            if (dstSnap) {
              std::atomic_store_explicit(&dst->clipSnapshot, dstSnap,
                                         std::memory_order_release);
            }
            EngineUndoEntry e;
            e.structural = true;
            e.trackId = srcId;
            e.before = std::move(srcBefore);
            e.after = snapshotTrackStore(*src);
            e.hasSecond = true;
            e.secondTrackId = dstId;
            e.secondBefore = std::move(dstBefore);
            e.secondAfter = snapshotTrackStore(*dst);
            pushUndo(std::move(e));
            ok = true;
          }
        }
        if (ok) {
          // Both lanes changed, so both bases must move — advancing only the source
          // would leave an author on the destination track accepted against a base
          // that no longer describes its placements.
          bumpClipVersionFor(src);
          bumpClipVersionFor(dst);
          clipDirty.store(true, std::memory_order_release);
        }
        std::cout << "UI: MovePlacement " << placementId << " cross-track " << srcId
                  << " -> " << dstId << (ok ? "" : " (failed)") << std::endl;
      } else {
        const bool ok = applyPlacementEdit(
            payload.trackId, [&](std::vector<daw::ProjectPlacement>& pls) {
              for (auto& p : pls) {
                if (p.id == placementId) {
                  p.at = newAt;
                  return true;
                }
              }
              return false;
            });
        std::cout << "UI: MovePlacement " << placementId << " -> at " << newAt
                  << (ok ? "" : " (not found)") << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RemovePlacement)) {
      const uint32_t placementId = payload.value0;
      const bool ok = applyPlacementEdit(
          payload.trackId, [&](std::vector<daw::ProjectPlacement>& pls) {
            for (auto it = pls.begin(); it != pls.end(); ++it) {
              if (it->id == placementId) {
                pls.erase(it);
                return true;
              }
            }
            return false;
          });
      std::cout << "UI: RemovePlacement " << placementId << (ok ? "" : " (not found)")
                << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::ResizePlacement)) {
      // Both start (`at`) and length in one op; 0xFFFF... = leave that field unchanged, so
      // a left-edge trim sends both and a right-edge drag sends length + at=sentinel.
      const uint32_t placementId = payload.value0;
      const uint64_t newAt = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                             payload.noteNanotickLo;
      const uint64_t newLen = (static_cast<uint64_t>(payload.noteDurationHi) << 32) |
                              payload.noteDurationLo;
      const bool ok = applyPlacementEdit(
          payload.trackId, [&](std::vector<daw::ProjectPlacement>& pls) {
            for (auto& p : pls) {
              if (p.id == placementId) {
                if (newAt != kPlacementUnchanged) {
                  p.at = newAt;
                }
                if (newLen != kPlacementUnchanged) {
                  p.lengthNanoticks = newLen;
                }
                return true;
              }
            }
            return false;
          });
      std::cout << "UI: ResizePlacement " << placementId << (ok ? "" : " (not found)")
                << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::AddPlacement)) {
      // Place an existing clip (value0 = clipId) at `at` for `length`. The clip must be
      // owned by the track for its content to resolve; an unknown id yields an empty box.
      const uint32_t clipId = payload.value0;
      const uint64_t at = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                          payload.noteNanotickLo;
      const uint64_t len = (static_cast<uint64_t>(payload.noteDurationHi) << 32) |
                           payload.noteDurationLo;
      // kPlacementUnchanged is Resize's "leave this field alone" sentinel. It is
      // meaningless for an ADD, and accepting it created a placement at tick 2^64-1 —
      // an invisible box at the end of time that then poisoned any song-end computation
      // that added a length to it. Refuse it, and say so.
      if (at == kPlacementUnchanged || len == kPlacementUnchanged) {
        daw::LogLine() << "UI: AddPlacement rejected — `at` and `length` are required "
                     "(0xFFFF..FF is Resize's leave-unchanged sentinel, not a position)"
                  << std::endl;
        DAW_EVENT("placement.add_rejected")
            .field("track", payload.trackId)
            .field("clip", clipId)
            .field("reason", "sentinel_position");
        return;
      }
      const uint32_t newId = nextPlacementId.fetch_add(1, std::memory_order_relaxed);
      applyPlacementEdit(payload.trackId,
                         [&](std::vector<daw::ProjectPlacement>& pls) {
                           daw::ProjectPlacement p;
                           p.clipId = clipId;
                           p.id = newId;
                           p.at = at;
                           p.lengthNanoticks = len;
                           pls.push_back(std::move(p));
                           return true;
                         });
      std::cout << "UI: AddPlacement clip " << clipId << " -> placement " << newId
                << " at " << at << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::TogglePlay)) {
      const bool next = !playing.load(std::memory_order_acquire);
      playing.store(next, std::memory_order_release);
      std::cout << "UI: Transport " << (next ? "Play" : "Pause") << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Stop)) {
      // Halt and rewind to the loop start. resetTimeline is drained by the
      // producer, which rewinds the transport and the audio playback position
      // together so the next Play starts clean.
      playing.store(false, std::memory_order_release);
      resetTimeline.store(true, std::memory_order_release);
      // Flush any sustained preview notes: enqueue a note-off for every held pitch so a
      // dropped keyup (or a Stop mid-audition) can't leave a stuck voice.
      {
        std::lock_guard<std::mutex> lock(previewMutex);
        for (auto& [trackId, held] : heldPreview) {
          for (const uint8_t pitch : held) {
            pendingPreviewNotes.push_back({trackId, pitch, 0, false});
          }
          held.clear();
        }
      }
      std::cout << "UI: Transport Stop" << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Panic)) {
      // PANIC: cut everything. Stop halts and flushes held KEYJAZZ notes, which is right
      // but is not a panic — it cannot reach a plugin's own ringing voices, a sequencer
      // note whose note-off has not been reached, or a generator mid-phrase. This raises
      // the flag the producer turns into CC120 (all-sound-off) + CC123 (all-notes-off) on
      // every channel of every hosted plugin, and drops the engine's own note bookkeeping.
      // Also halt: a panic that leaves the sequencer running would immediately re-trigger.
      playing.store(false, std::memory_order_release);
      panicPending.store(true, std::memory_order_release);
      // Drop held preview state outright. The CC120 below already cuts those voices, so
      // enqueuing note-offs for them would be redundant — and leaving them held would let
      // a later Stop emit note-offs for pitches that no longer sound.
      {
        std::lock_guard<std::mutex> lock(previewMutex);
        pendingPreviewNotes.clear();
        heldPreview.clear();
      }
      // And the part a controller message cannot reach: reset every hosted plugin's own
      // DSP state. CC120 asks a plugin to stop sounding; a voice wedged inside the
      // plugin's state ignores it, which is precisely the case panic exists for. Sent on
      // the control socket (off the RT path) to every track host AND the master's, so a
      // master-chain plugin is covered too.
      uint32_t resetHosts = 0;
      {
        std::vector<TrackRuntime*> all;
        {
          std::lock_guard<std::mutex> lock(tracksMutex);
          for (auto& rt : tracks) {
            if (rt) {
              all.push_back(rt.get());
            }
          }
        }
        if (masterTrack) {
          all.push_back(masterTrack.get());
        }
        for (auto* rt : all) {
          if (!rt->hostReady.load(std::memory_order_acquire)) {
            continue;
          }
          std::lock_guard<std::mutex> lock(rt->controllerMutex);
          if (rt->controller.sendResetPlugins()) {
            ++resetHosts;
          }
        }
      }
      DAW_EVENT("transport.panic").field("hosts_reset", static_cast<uint64_t>(resetHosts));
      std::cout << "UI: PANIC — all sound off (" << resetHosts
                << " host(s) reset)" << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetPosition)) {
      const uint64_t target =
          static_cast<uint64_t>(payload.noteNanotickLo) |
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
      const auto loop = daw::engine::effectiveLoop(
          loopStartNanotick.load(std::memory_order_acquire),
          loopEndNanotick.load(std::memory_order_acquire), patternTicks);
      // CLAMPED, NOT WRAPPED, and the two are a deliberate pair — see clampTickIntoLoop.
      const uint64_t clamped =
          daw::engine::clampTickIntoLoop(target, loop.startTick, loop.endTick);
      transportNanotick.store(clamped, std::memory_order_release);
      // A SEEK RESTARTS THE PASS COUNT. Carrying it across a seek would make a conditional trig
      // depend on how the playhead got here, which is exactly the "depends on the session's
      // history" property that makes a bounce irreproducible.
      transportElapsedNanotick.store(0, std::memory_order_release);
      std::cout << "UI: Transport SetPosition " << clamped << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestChainSnapshot)) {
      daw::engine::handleRequestChainSnapshot(requestCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Quit)) {
      // The last UI went away. Silence first, then exit: `running` unwinds through
      // the join/stop path at the bottom of main(), which takes a moment, and a
      // moment of audio after the window closed is exactly what this exists to
      // stop. The sidecar only sends this after a grace period, so a page reload
      // does not end the session.
      playing.store(false, std::memory_order_release);
      std::cout << "UI: last client gone — engine shutting down" << std::endl;
      running.store(false, std::memory_order_release);
      restartCv.notify_all();
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTempo)) {
      // value0 = milli-BPM. flags: 1 = flatten the map to this single tempo (a
      // transport-bar BPM edit); 0 = insert-or-replace a point at the nanotick in
      // noteNanotickLo/Hi (a tempo-lane edit). Runs on the UI command thread, same as
      // load/save, so loadedTempoMap is single-threaded here; setMap is mutex-guarded
      // against the UI-publish reader. Save re-emits loadedTempoMap, so this persists.
      const double bpm = static_cast<double>(payload.value0) / 1000.0;
      if (bpm > 0.0) {
        if (payload.flags == 1) {
          loadedTempoMap = {{0, bpm}};
        } else {
          const uint64_t pos =
              static_cast<uint64_t>(payload.noteNanotickLo) |
              (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
          bool replaced = false;
          for (auto& pt : loadedTempoMap) {
            if (pt.nanotick == pos) {
              pt.bpm = bpm;
              replaced = true;
              break;
            }
          }
          if (!replaced) {
            loadedTempoMap.push_back({pos, bpm});
          }
          // Keep the retained map sorted by position so a save re-emits an ordered
          // tempo_map (the provider sorts its own copy, but loadedTempoMap is what
          // SaveProject writes out).
          std::sort(loadedTempoMap.begin(), loadedTempoMap.end(),
                    [](const daw::ProjectTempoPoint& a,
                       const daw::ProjectTempoPoint& b) {
                      return a.nanotick < b.nanotick;
                    });
        }
        std::vector<daw::TempoPoint> pts;
        pts.reserve(loadedTempoMap.size());
        for (const auto& pt : loadedTempoMap) {
          pts.push_back({pt.nanotick, pt.bpm});
        }
        tempoProvider.setMap(std::move(pts));
        std::cout << "UI: SetTempo " << bpm << " bpm (flags " << payload.flags
                  << ")" << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetDeviceParam)) {
      daw::engine::handleSetDeviceParam(deviceCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestDeviceParams)) {
      daw::engine::handleRequestDeviceParams(requestCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestWaveform)) {
      daw::engine::handleRequestWaveform(requestCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestClipWindow)) {
      daw::engine::handleRequestClipWindow(requestCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Undo)) {
      daw::engine::handleUndo(undoCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Redo)) {
      daw::engine::handleRedo(undoCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackMixer)) {
      daw::engine::handleSetTrackMixer(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::WriteHarmony)) {
      daw::engine::handleWriteHarmony(noteCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::DeleteHarmony)) {
      daw::engine::handleDeleteHarmony(noteCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::WriteChord) ||
               payload.commandType == static_cast<uint16_t>(daw::UiCommandType::DeleteChord)) {
      daw::engine::handleWriteChord(noteCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackHarmonyQuantize)) {
      daw::engine::handleSetTrackHarmonyQuantize(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackSoundAddressed)) {
      daw::engine::handleSetTrackSoundAddressed(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackCollapsed)) {
      daw::engine::handleSetTrackCollapsed(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackLinesPerBeat)) {
      daw::engine::handleSetTrackLinesPerBeat(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackAllowNoteOverlap)) {
      daw::engine::handleSetTrackAllowNoteOverlap(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetLaneQuantize)) {
      daw::engine::handleSetLaneQuantize(trackpropsCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetLoopRange)) {
      const uint64_t start =
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
          payload.noteNanotickLo;
      const uint64_t end =
          (static_cast<uint64_t>(payload.noteDurationHi) << 32) |
          payload.noteDurationLo;
      if (end > start) {
        loopStartNanotick.store(start, std::memory_order_release);
        loopEndNanotick.store(end, std::memory_order_release);
        // Set whenever the loop IS set, not only when the playhead had to move with it:
        // this is what stops a later placement edit from silently taking the loop back.
        loopUserSet.store(true, std::memory_order_release);
        uint64_t current =
            transportNanotick.load(std::memory_order_acquire);
        if (current < start || current >= end) {
          transportNanotick.store(start, std::memory_order_release);
        }
        std::cout << "UI: Loop range set [" << start << ", " << end << ")"
                  << std::endl;
      } else {
        daw::LogLine() << "UI: Invalid loop range [" << start << ", " << end << ")"
                  << std::endl;
      }
    }
}

}  // namespace daw::engine
