#include "apps/engine_arrangetime_commands.h"

#include <cstring>

#include "apps/event_log.h"
#include "apps/ripple.h"

namespace daw::engine {

void handleArrangeTime(ArrangeTimeCommandDeps& deps,
            const daw::EventEntry& entry,
            daw::UiCommandType commandType) {
  auto& arrangeMutex = deps.engineState.arrange.arrangeMutex;
  auto& arrangeVersion = deps.engineState.arrange.arrangeVersion;
  auto& automationVersion = deps.automationVersion;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& clipDirty = deps.clipDirty;
  auto& harmonyDirty = deps.harmonyTimeline.harmonyDirty;
  auto& harmonyEvents = deps.harmonyTimeline.harmonyEvents;
  auto& harmonyMutex = deps.harmonyTimeline.harmonyMutex;
  auto& harmonyVersion = deps.harmonyTimeline.harmonyVersion;
  auto& historyAppend = deps.historyAppend;
  auto& loadedTempoMap = deps.engineState.songTiming.loadedTempoMap;
  auto& markerList = deps.engineState.arrange.markerList;
  auto& meterSnapshot = deps.engineState.songTiming.meterSnapshot;
  auto& pushUndo = deps.pushUndo;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& recomputeSongEnd = deps.recomputeSongEnd;
  auto& snapshotSongStore = deps.snapshotSongStore;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& songMeter = deps.engineState.arrange.songMeter;
  auto& songTimeSigDen = deps.engineState.songTiming.songTimeSigDen;
  auto& songTimeSigNum = deps.engineState.songTiming.songTimeSigNum;
  auto& tempoProvider = deps.tempoProvider;

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
        // THE METER IS RIPPLED, twenty-seven lines above this comment. That is worth stating
        // here because this paragraph used to say the opposite — "THE METER NEEDS NO RIPPLE AT
        // ALL … the meter now lives ON the section, so a section carries its meter with it by
        // construction and there is nothing to move. The question is not answered, it is
        // dissolved." That was true of the Section spine, and the spine was deleted in v29
        // (`bb0471bb`): the meter is now an authoritative tick-keyed map, so inserting bars
        // before a meter change has to carry it exactly as it carries a tempo change.
        //
        // The stale sentence survived inside the very function that refutes it, and the loop
        // that does the rippling says so in its own comment — "the spine could not have this
        // bug … and could not have the capability either". A superseded rule left standing next
        // to its replacement is the shape this project keeps paying for; it is kept here as a
        // disavowal rather than deleted, so a reader who learnt the old rule sees it retired.
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

}  // namespace daw::engine
