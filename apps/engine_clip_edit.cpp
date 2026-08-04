#include "engine_clip_edit.h"

// What the five bodies reach for beyond the module header. The file arrived carrying
// main.cpp's 98 includes, which described where it used to live rather than what it uses.
#include "engine_pure.h"
#include "event_log.h"


namespace daw::engine {

// TWO HELPERS THAT CAME WITH THEIR ONLY CALLERS. Both were lambdas in main() used by nothing except
// the three remove-edits below, so they are file-scope here rather than entries in ClipEditDeps: a
// dependency that exists to serve one module is that module's business. firstPlacementAtForClip
// captures nothing at all.
namespace {

uint64_t firstPlacementAtForClip(const TrackRuntime& rt, uint32_t clipId) {

    for (const auto& pl : rt.sourcePlacements) {
      if (pl.clipId == clipId && pl.at.has_value()) {
        return *pl.at;
      }
    }
    return 0;
}

}  // namespace

static bool emitRemoveChordDiff(ClipEditDeps& deps, uint32_t trackId, const daw::MusicalClip::RemovedChord& removed, uint64_t absTick) {
  auto bumpTrackClipVersion = [&](auto&&... a) { return daw::engine::bumpTrackClipVersion(deps, decltype(a)(a)...); };
  auto& clipDirty = deps.clipDirty;
  auto& emitChordDiff = deps.emitChordDiff;

    clipDirty.store(true, std::memory_order_release);
    const uint32_t nextClipVersion = bumpTrackClipVersion(trackId);
    daw::UiChordDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiChordDiffType::RemoveChord);
    diffPayload.trackId = trackId;
    diffPayload.clipVersion = nextClipVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(absTick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((absTick >> 32) & 0xffffffffu);
    diffPayload.durationLo = static_cast<uint32_t>(removed.duration & 0xffffffffu);
    diffPayload.durationHi = static_cast<uint32_t>((removed.duration >> 32) & 0xffffffffu);
    diffPayload.chordId = removed.chordId;
    diffPayload.spreadNanoticks =
        (static_cast<uint32_t>(removed.column) << 24) |
        (removed.spreadNanoticks & 0x00ffffffu);
    diffPayload.packed = static_cast<uint32_t>(removed.degree) |
                         (static_cast<uint32_t>(removed.quality) << 8) |
                         (static_cast<uint32_t>(removed.inversion) << 16) |
                         (static_cast<uint32_t>(removed.baseOctave) << 24);
    diffPayload.flags = static_cast<uint16_t>(removed.humanizeTiming & 0xffu) |
                        static_cast<uint16_t>((removed.humanizeVelocity & 0xffu) << 8);
    emitChordDiff(diffPayload);
    return true;
}


EditTarget locateEditTarget(LocateTargetDeps& deps, TrackRuntime& rt, uint64_t absTick,
                            bool createIfMissing) {
  auto& nextClipId = deps.nextClipId;
  auto& nextPlacementId = deps.nextPlacementId;
  auto& songBarGrid = deps.songBarGrid;

    // THE STRETCH THRESHOLD is still one 4/4 bar, and that one is a FEEL setting — how far past
    // a clip's end you can keep typing before a new clip starts — rather than a statement about
    // the ruler. The ANCHOR, which is a statement about the ruler, now comes from the song's
    // meter via songBarGrid (task #43).
    const uint64_t bar = 4 * daw::NanotickConverter::kNanoticksPerQuarter;
    std::vector<daw::PlacementSpan> spans;
    for (size_t i = 0; i < rt.sourcePlacements.size(); ++i) {
      const auto& pl = rt.sourcePlacements[i];
      if (!pl.at.has_value()) {
        continue;
      }
      uint64_t clipLen = 0;
      uint64_t contentEnd = 0;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clipLen = c.lengthNanoticks;
          contentEnd = clipContentEnd(c.clip);
          break;
        }
      }
      // Coverage extent for the note-entry rule: an explicit placement length,
      // else the clip's own loop length, else (a linear length-0 clip) its
      // content extent — the same span a save would segment, so live entry and a
      // later reload agree on where one clip ends and the next begins.
      const uint64_t effLen = pl.lengthNanoticks > 0
                                  ? pl.lengthNanoticks
                                  : (clipLen > 0 ? clipLen : contentEnd);
      spans.push_back(daw::PlacementSpan{*pl.at, effLen, clipLen, i});
    }
    const auto decision = daw::resolveNoteEntry(spans, absTick, bar, songBarGrid());

    auto findOwned = [&](uint32_t clipId) -> size_t {
      for (size_t i = 0; i < rt.ownedClips.size(); ++i) {
        if (rt.ownedClips[i].id == clipId) {
          return i;
        }
      }
      return rt.ownedClips.size();
    };

    EditTarget t;
    if (decision.kind == daw::NoteEntryKind::CreateNew) {
      if (!createIfMissing) {
        return t;  // a remove outside every clip: nothing to do
      }
      daw::ProjectClip nc;
      nc.id = nextClipId.fetch_add(1, std::memory_order_acq_rel);
      nc.name = "Clip";
      nc.lengthNanoticks = 0;
      // Inherit the grid of the predecessor clip on this track — the placement with
      // the greatest anchor before the new one — so a new section keeps the meter you
      // were working in rather than snapping back to 4/4. Defaults stand when there is
      // no predecessor.
      {
        const daw::ProjectClip* pred = nullptr;
        uint64_t predAt = 0;
        for (const auto& p : rt.sourcePlacements) {
          if (!p.at.has_value() || *p.at >= decision.at) {
            continue;
          }
          if (pred == nullptr || *p.at > predAt) {
            const size_t oi = findOwned(p.clipId);
            if (oi < rt.ownedClips.size()) {
              pred = &rt.ownedClips[oi];
              predAt = *p.at;
            }
          }
        }
        if (pred != nullptr) {
          nc.linesPerBeat = pred->linesPerBeat;
          nc.timeSigNumerator = pred->timeSigNumerator;
          nc.timeSigDenominator = pred->timeSigDenominator;
        }
      }
      const uint32_t newId = nc.id;
      rt.ownedClips.push_back(std::move(nc));
      rt.editableClipIds.push_back(newId);
      daw::ProjectPlacement pl;
      pl.clipId = newId;
      pl.id = nextPlacementId.fetch_add(1, std::memory_order_relaxed);  // stable id
      pl.at = decision.at;
      pl.lengthNanoticks = 0;
      rt.sourcePlacements.push_back(std::move(pl));
      t.valid = true;
      t.ownedIndex = rt.ownedClips.size() - 1;
      t.relTick = decision.clipRelativeTick;
      t.placementIndex = rt.sourcePlacements.size() - 1;
      t.clipId = newId;
      t.placementAt = decision.at;
      return t;
    }
    // InsidePlacement / StretchPlacement: the covering placement's owned clip.
    const size_t pi = decision.placementIndex;
    if (pi >= rt.sourcePlacements.size()) {
      return t;  // invalid
    }
    const uint32_t clipId = rt.sourcePlacements[pi].clipId;
    const size_t oi = findOwned(clipId);
    if (oi >= rt.ownedClips.size()) {
      return t;  // no owned clip for this placement (should not happen)
    }
    t.valid = true;
    t.ownedIndex = oi;
    t.relTick = decision.clipRelativeTick;
    t.placementIndex = pi;
    t.clipId = clipId;
    t.placementAt = *rt.sourcePlacements[pi].at;
    return t;
}

bool applyAddNote(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint64_t duration,
                  uint8_t pitch, uint8_t velocity, uint16_t flags, bool recordUndo,
                  std::optional<daw::EventId> noteIdOverride, uint16_t sound,
                  uint16_t soundOffset) {
  auto& barEndTick = deps.barEndTick;
  auto& clipDirty = deps.clipDirty;
  auto& clipVersion = deps.clipVersion;
  auto& commitStructuralEdit = deps.commitStructuralEdit;
  auto consumeClipVersionForNoOp = [&](auto&&... a) { return daw::engine::consumeClipVersionForNoOp(deps, decltype(a)(a)...); };
  auto& emitUiDiff = deps.emitUiDiff;
  auto forkOwnedClip = [&](auto&&... a) { return daw::engine::forkOwnedClip(deps, decltype(a)(a)...); };
  auto growLengthsForContent = [&](auto&&... a) { return daw::engine::growLengthsForContent(deps, decltype(a)(a)...); };
  auto& locateEditTarget = deps.locateEditTarget;
  auto& loopEndNanotick = deps.transport.loopEndNanotick;
  auto& loopStartNanotick = deps.transport.loopStartNanotick;
  auto& patternTicks = deps.patternTicks;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: AddNote failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    // A note with no velocity and no length is an OFF gesture. It ends the
    // note sounding in that column rather than storing an event of its own,
    // so length has exactly one representation.
    const uint8_t column = static_cast<uint8_t>(flags & 0xffu);
    const bool isNoteOff = velocity == 0 && duration == 0;

    // Where a note runs to when nothing follows it in its column. A note entered
    // PAST the current span (loop/pattern end) still needs room to sound, or it
    // lands with zero length and vanishes — so the span always reaches at least
    // the end of the bar containing the note. Writing past the end grows the song.
    uint64_t spanEnd = loopEndNanotick.load(std::memory_order_acquire);
    if (spanEnd <= loopStartNanotick.load(std::memory_order_acquire)) {
      spanEnd = patternTicks;
    }
    {
      spanEnd = std::max(spanEnd, barEndTick(nanotick));
    }

    std::optional<daw::ClipEditResult> result;
    std::shared_ptr<const ClipSnapshot> snapshot;
    bool noOp = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // Structural store: the edit targets the owned clip covering this tick
      // (a new clip for a note-on out on its own), at the clip-relative tick.
      // track.clip is re-derived from the store afterward, so the audio thread
      // and the UI see exactly the same flat result as before this reroute.
      TrackStoreState before = snapshotTrackStore(*runtime);
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/!isNoteOff);
      if (!target.valid) {
        // An OFF (or edit) with no clip to land in — nothing to do.
        consumeClipVersionForNoOp(runtime);
        noOp = true;
      } else {
        const uint64_t relSpanEnd =
            spanEnd > target.placementAt ? spanEnd - target.placementAt : 0;
        daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
        if (isNoteOff) {
          result = daw::endNoteInColumn(clip, trackId, target.relTick, column,
                                        runtime->trackClipVersion, recordUndo);
          if (!result) {
            consumeClipVersionForNoOp(runtime);
            noOp = true;
          }
        } else {
          result = daw::addNoteToClip(
              clip, trackId, target.relTick, duration, pitch, velocity, flags,
              runtime->trackClipVersion, recordUndo, relSpanEnd, noteIdOverride,
              sound, soundOffset,
              runtime->allowNoteOverlap.load(std::memory_order_relaxed));
        }
        if (result) {
          // HOW MANY APPEARANCES THIS EDIT REACHED. A clip is CONTENT and a placement is an
          // APPEARANCE, so an edit to a clip placed four times changes all four — that is the
          // Movement 3 promise ("fix the bass in chorus 1 and all three choruses change") and it
          // is also the most surprising thing in the program if nothing says it out loud. Two
          // placements of one clip draw as two identical rails; a note typed into one silently
          // rewrites the other, and there is no moment where the model announces itself.
          //
          // Counted HERE, where the answer is exact, rather than left to a client to infer by
          // grouping extents by clip id. On the event stream and in history.jsonl, so "this
          // changed 4 placements" is available to a console, to the linter, and to whoever reads
          // the journal a week later asking what happened.
          //
          // Counted AFTER forkOwnedClip: a copy-on-write fork repoints this track's placements,
          // so a count taken before it would be the pre-fork clip's.
          forkOwnedClip(*runtime, target.ownedIndex);
          if (target.ownedIndex < runtime->ownedClips.size()) {
            const uint32_t editedClipId = runtime->ownedClips[target.ownedIndex].id;
            uint32_t appearances = 0;
            for (const auto& pl : runtime->sourcePlacements) {
              if (pl.clipId == editedClipId) {
                ++appearances;
              }
            }
            if (appearances > 1) {
              DAW_EVENT("clip.shared_edit")
                  .field("track", trackId)
                  .field("clip", editedClipId)
                  .field("nanotick", nanotick)
                  .field("placements_affected", appearances);
            }
          }
          growLengthsForContent(*runtime, target);
          shiftDiffTick(result->diff, target.placementAt);
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
        }
      }
    }
    if (!result) {
      (void)noOp;
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    // The store already advanced this track's version and stamped result->diff with it;
    // the global gate moves AFTER, so a publisher that sees the new gate is guaranteed
    // to read the new per-track value. See bumpClipVersionFor for why the order matters.
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);
    return true;
}

bool applySetRowOps(ClipEditDeps& deps, uint32_t trackId, uint32_t clipId, daw::EventId noteId,
                    const daw::RowOpEdit& edit, bool recordUndo,
                    daw::UiClipRejectReason& rejectReason) {
  auto bumpClipVersionFor = [&](auto&&... a) { return daw::engine::bumpClipVersionFor(deps, decltype(a)(a)...); };
  auto& clipDirty = deps.clipDirty;
  auto& clipVersion = deps.clipVersion;
  auto& emitUiDiff = deps.emitUiDiff;
  auto forkOwnedClip = [&](auto&&... a) { return daw::engine::forkOwnedClip(deps, decltype(a)(a)...); };
  auto& pushStructuralUndo = deps.pushStructuralUndo;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      DAW_EVENT("rowops.rejected")
          .field("track", trackId)
          .field("note", static_cast<uint64_t>(noteId))
          .field("reason", "no_such_track");
      rejectReason = daw::UiClipRejectReason::UnknownTrack;
      return false;
    }

    std::optional<daw::ClipEditResult> result;
    std::shared_ptr<const ClipSnapshot> snapshot;
    uint64_t placementAt = 0;
    // Whether the note was found as a PLACEMENT OVERRIDE rather than in a clip. Declared out here
    // because the commit tail below has to know: an override edit produces no ClipEditResult, so
    // "no result" alone cannot distinguish success from refusal.
    bool editedOverride = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      size_t ownedIndex = runtime->ownedClips.size();
      for (size_t i = 0; i < runtime->ownedClips.size(); ++i) {
        if (clipId != 0 && runtime->ownedClips[i].id != clipId) {
          continue;
        }
        if (runtime->ownedClips[i].clip.findNoteById(noteId) != nullptr) {
          ownedIndex = i;
          break;
        }
      }
      if (ownedIndex >= runtime->ownedClips.size()) {
        // NOT IN A CLIP — TRY THE PLACEMENT OVERRIDES.
        //
        // A note does not only live in a clip. A placement can carry LOCAL edits
        // (ProjectPlacement::adds, serialised as "notes"), and flattenPlacements publishes them
        // to the UI exactly like clip notes — same rail, same ids, indistinguishable on the wire.
        // Searching only ownedClips meant an editor could SEE a note it could not edit, and be
        // told "no_such_note" about a note plainly on screen. Reported by the web-UI agent as
        // "SetRowOps only reaches notes created this session"; the real boundary was not session
        // or ownership but WHICH CONTAINER the note ended up in.
        for (auto& pl : runtime->sourcePlacements) {
          if (clipId != 0 && pl.clipId != clipId) {
            continue;
          }
          for (auto& ev : pl.adds) {
            if (ev.type != daw::MusicalEventType::Note ||
                ev.payload.note.noteId != noteId) {
              continue;
            }
            if (!daw::applyRowOpEdit(ev.payload.note, edit)) {
              DAW_EVENT("rowops.rejected")
                  .field("track", trackId)
                  .field("note", static_cast<uint64_t>(noteId))
                  .field("reason", "out_of_range");
              rejectReason = daw::UiClipRejectReason::ValueOutOfRange;
              return false;
            }
            placementAt = pl.at ? *pl.at : 0;
            runtime->arrangementDirty.store(true, std::memory_order_relaxed);
            snapshot = rebuildFlatAndPublish(*runtime);
            if (recordUndo) {
              pushStructuralUndo(trackId, std::move(before), snapshotTrackStore(*runtime));
            }
            editedOverride = true;
            break;
          }
          if (editedOverride) {
            break;
          }
        }
        if (!editedOverride) {
          DAW_EVENT("rowops.rejected")
              .field("track", trackId)
              .field("clip", clipId)
              .field("note", static_cast<uint64_t>(noteId))
              .field("reason", "no_such_note");
          rejectReason = daw::UiClipRejectReason::UnknownNote;
          return false;
        }
      }
      // Where this clip sits, so the diff's tick is on the timeline rather than clip-relative —
      // the same shiftDiffTick a note edit does.
      if (!editedOverride) {
      for (const auto& pl : runtime->sourcePlacements) {
        if (pl.clipId == runtime->ownedClips[ownedIndex].id && pl.at) {
          placementAt = *pl.at;
          break;
        }
      }
      result = daw::setNoteRowOps(runtime->ownedClips[ownedIndex].clip, trackId, noteId,
                                  edit, runtime->trackClipVersion, recordUndo);
      }
      if (result) {
        forkOwnedClip(*runtime, ownedIndex);
        runtime->arrangementDirty.store(true, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        if (recordUndo) {
          pushStructuralUndo(trackId, std::move(before), snapshotTrackStore(*runtime));
        }
      }
    }
    if (!result && !editedOverride) {
      DAW_EVENT("rowops.rejected")
          .field("track", trackId)
          .field("note", static_cast<uint64_t>(noteId))
          .field("reason", "out_of_range");
      rejectReason = daw::UiClipRejectReason::ValueOutOfRange;
      return false;
    }
    if (result) {
      shiftDiffTick(result->diff, placementAt);
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipVersion.fetch_add(1, std::memory_order_acq_rel);  // see applyAddNote for the order
    clipDirty.store(true, std::memory_order_release);
    if (result) {
      emitUiDiff(result->diff);
    } else {
      // An override edit produces no ClipEditResult (the store helpers take a MusicalClip), so
      // the version bump that a clip edit gets from setNoteRowOps is done here instead. Without
      // it the flat clip is republished with a version the UI has already seen and the edit
      // never reaches the screen.
      bumpClipVersionFor(runtime);
    }
    DAW_EVENT("rowops.set")
        .field("track", trackId)
        .field("note", static_cast<uint64_t>(noteId))
        .field("mask", static_cast<uint64_t>(edit.mask));
    return true;
}

bool applyLocalNoteEdit(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick,
                        uint64_t duration, uint8_t pitch, uint8_t velocity, uint8_t column,
                        bool deleting,
                        const std::function<PlacementHit(TrackRuntime&, uint64_t)>&
                            findPlacementAt) {
  auto& barEndTick = deps.barEndTick;
  auto bumpClipVersionFor = [&](auto&&... a) { return daw::engine::bumpClipVersionFor(deps, decltype(a)(a)...); };
  auto& clipDirty = deps.clipDirty;
  auto& nextClipId = deps.nextClipId;
  auto& pushStructuralUndo = deps.pushStructuralUndo;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      return false;
    }
    // THE OFF GESTURE IS NOT AN OVERRIDE. Velocity 0 with length 0 means "end the note
    // sounding in this column" — it ends something on the flat stream and stores no event of
    // its own, which is a clip-level operation. Routed through here it became an ADD carrying
    // velocity 0 and length 0: a phantom that can never sound, is saved, and counts toward the
    // override badge. Refuse it and say so; silently storing a note-shaped nothing is worse
    // than answering no. Checked BEFORE the length default below, which would otherwise erase
    // the very thing that identifies the gesture.
    if (!deleting && velocity == 0 && duration == 0) {
      DAW_EVENT("local_edit.rejected")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("reason", "note_off_needs_clip_scope");
      return false;
    }
    bool changed = false;
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // UNDO. Overrides live ON the placement, so the ordinary store snapshot already
      // carries them — and this pushed nothing, which was worse than it sounds. Undo here
      // is a whole-store SWAP, not a per-edit inverse: type a note, add a local hat, press
      // Ctrl-Z, and the entry that pops is the note's, restoring the store from before the
      // note — taking the hat with it. Redo re-applies the note's after-state, which also
      // predates the hat. So one undo destroyed the override and redo could not bring it
      // back. Recorded exactly like applyPlacementEdit below.
      TrackStoreState storeBefore = snapshotTrackStore(*runtime);
      // Which APPEARANCE is this tick in? A local edit is meaningless without one: there
      // is no placement to hang the override on, so it is refused rather than silently
      // becoming a clip edit — which would be the opposite of what was asked for.
      const PlacementHit hit = findPlacementAt(*runtime, nanotick);
      daw::ProjectPlacement* target = hit.placement;
      const uint64_t targetEnd = hit.end;
      if (!target) {
        DAW_EVENT("local_edit.rejected")
            .field("track", trackId)
            .field("nanotick", nanotick)
            .field("reason", "no_placement_here");
        return false;
      }
      // Overrides are PLACEMENT-RELATIVE, so they survive the placement being moved —
      // that is the difference between "the hat in chorus 3" and "a hat at bar 27".
      const uint64_t rel = nanotick - *target->at;
      // A CLIP SHORTER THAN ITS PLACEMENT LOOPS, and the base notes only exist once — at
      // offsets inside the clip. So the base-note lookup below needs the CLIP-relative tick,
      // not the placement-relative one: with a 1-bar clip across 4 bars, a local delete
      // anywhere in bars 2-4 compared rel (>= one bar) against every event's offset (< one
      // bar), matched nothing, muted nothing, and returned false without a word. Adds are
      // unaffected — they are placement-relative by design, which is what lets an add live
      // past the clip's length instead of repeating with it.
      //
      // Muting by note id silences that clip note in EVERY iteration of this appearance,
      // which is what the additive-only model can express: the override belongs to the
      // appearance, and within the appearance the note recurs. Per-iteration muting would
      // need the mute record to carry an iteration index.
      uint64_t clipLen = 0;
      for (const auto& c : runtime->ownedClips) {
        if (c.id == target->clipId) {
          clipLen = c.lengthNanoticks;
          break;
        }
      }
      const uint64_t clipRel = (clipLen > 0 && rel >= clipLen) ? (rel % clipLen) : rel;
      if (deleting) {
        // Deleting an ADD removes it; deleting a BASE note mutes it. Two different
        // records for what looks like one gesture, because the base note is not ours to
        // remove — the clip may be placed elsewhere and still want it.
        const size_t before = target->adds.size();
        target->adds.erase(
            std::remove_if(target->adds.begin(), target->adds.end(),
                           [&](const daw::MusicalEvent& e) {
                             return e.type == daw::MusicalEventType::Note &&
                                    e.nanotickOffset == rel &&
                                    e.payload.note.pitch == pitch &&
                                    e.payload.note.column == column;
                           }),
            target->adds.end());
        if (target->adds.size() != before) {
          changed = true;
        } else {
          // Not an add — find the base note and mute it by id.
          for (const auto& c : runtime->ownedClips) {
            if (c.id != target->clipId) {
              continue;
            }
            for (const auto& e : c.clip.events()) {
              if (e.type != daw::MusicalEventType::Note ||
                  e.nanotickOffset != clipRel ||
                  e.payload.note.pitch != pitch ||
                  e.payload.note.column != column) {
                continue;
              }
              const daw::EventId id = e.payload.note.noteId;
              if (std::find(target->mutes.begin(), target->mutes.end(), id) ==
                  target->mutes.end()) {
                target->mutes.push_back(id);
                changed = true;
              }
              break;
            }
            break;
          }
        }
      } else {
        // A NOTE WITH NO LENGTH NEVER SOUNDS: the scheduler skips a zero-duration event
        // outright and expandNoteOps returns no strikes. Clip scope already handles this — it
        // computes a span reaching at least the end of the bar containing the note, because
        // "a note entered past the current span still needs room to sound". Local scope stored
        // the 0 verbatim, so the SAME gesture produced a real note through one path and a
        // saved, badge-counted silence through the other. daw-cli defaults --duration to 0, so
        // `do note --local --pitch 60` was exactly that gesture.
        //
        // Same rule as clip scope, clamped to the appearance: an override belongs to this
        // placement and must not sound past it.
        uint64_t addDuration = duration;
        if (addDuration == 0) {
          const uint64_t barAfter = barEndTick(nanotick);
          addDuration = barAfter > nanotick
                            ? barAfter - nanotick
                            : 4 * daw::NanotickConverter::kNanoticksPerQuarter;
          if (targetEnd > nanotick && nanotick + addDuration > targetEnd) {
            addDuration = targetEnd - nanotick;
          }
        }
        daw::MusicalEvent add;
        add.nanotickOffset = rel;
        add.type = daw::MusicalEventType::Note;
        add.payload.note.pitch = pitch;
        add.payload.note.velocity = velocity;
        add.payload.note.column = column;
        add.payload.note.durationNanoticks = addDuration;
        // A local add gets its own note id from the clip's allocator space so it can be
        // addressed (and deleted) like any other note.
        add.payload.note.noteId = nextClipId.fetch_add(1, std::memory_order_relaxed);
        target->adds.push_back(std::move(add));
        changed = true;
      }
      if (changed) {
        runtime->arrangementDirty.store(true, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        pushStructuralUndo(trackId, std::move(storeBefore),
                           snapshotTrackStore(*runtime));
      }
    }
    if (!changed) {
      // A local edit that found its placement and then changed nothing used to return
      // silently — which is how the loop-repeat delete above stayed hidden. A gesture that
      // does nothing is worth one line: the caller cannot otherwise tell it from success.
      DAW_EVENT("local_edit.noop")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("pitch", static_cast<uint32_t>(pitch))
          .field("op", deleting ? "delete" : "add")
          .field("reason", deleting ? "no_add_or_base_note_matched" : "duplicate_add");
      return false;
    }
    if (snapshot) {
      std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                 std::memory_order_release);
    }
    bumpClipVersionFor(runtime);
    clipDirty.store(true, std::memory_order_release);
    DAW_EVENT("local_edit.applied")
        .field("track", trackId)
        .field("nanotick", nanotick)
        .field("op", deleting ? "override_removed_or_muted" : "added");
    return true;
}

bool applyAddChord(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint64_t duration,
                   uint8_t degree, uint8_t quality, uint8_t inversion, uint8_t baseOctave,
                   uint8_t column, uint32_t spreadNanoticks, uint16_t humanizeTiming,
                   uint16_t humanizeVelocity, bool recordUndo,
                   std::optional<uint32_t> chordIdOverride) {
  auto& barEndTick = deps.barEndTick;
  auto bumpClipVersionFor = [&](auto&&... a) { return daw::engine::bumpClipVersionFor(deps, decltype(a)(a)...); };
  auto& clipDirty = deps.clipDirty;
  auto& commitStructuralEdit = deps.commitStructuralEdit;
  auto consumeClipVersionForNoOp = [&](auto&&... a) { return daw::engine::consumeClipVersionForNoOp(deps, decltype(a)(a)...); };
  auto& emitChordDiff = deps.emitChordDiff;
  auto forkOwnedClip = [&](auto&&... a) { return daw::engine::forkOwnedClip(deps, decltype(a)(a)...); };
  auto growLengthsForContent = [&](auto&&... a) { return daw::engine::growLengthsForContent(deps, decltype(a)(a)...); };
  auto& locateEditTarget = deps.locateEditTarget;
  auto& loopEndNanotick = deps.transport.loopEndNanotick;
  auto& loopStartNanotick = deps.transport.loopStartNanotick;
  auto& nextChordId = deps.nextChordId;
  auto& patternTicks = deps.patternTicks;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: AddChord failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    daw::MusicalEvent event;
    event.nanotickOffset = nanotick;
    event.type = daw::MusicalEventType::Chord;
    uint32_t chordId = 0;
    if (chordIdOverride) {
      chordId = *chordIdOverride;
      uint32_t current = nextChordId.load(std::memory_order_acquire);
      while (current <= chordId) {
        const uint32_t desired = chordId + 1;
        if (nextChordId.compare_exchange_weak(current,
                                              desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          break;
        }
      }
    } else {
      chordId = nextChordId.fetch_add(1, std::memory_order_acq_rel);
    }
    event.payload.chord.chordId = chordId;
    event.payload.chord.degree = degree;
    event.payload.chord.quality = quality;
    event.payload.chord.inversion = inversion;
    event.payload.chord.baseOctave = baseOctave;
    event.payload.chord.column = column;
    event.payload.chord.spreadNanoticks = spreadNanoticks;
    event.payload.chord.humanizeTiming = humanizeTiming;
    event.payload.chord.humanizeVelocity = humanizeVelocity;
    std::shared_ptr<const ClipSnapshot> snapshot;
    uint64_t diffTick = nanotick;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      // Structural store: a chord lands in the owned clip covering this tick (a
      // fresh clip if it is out on its own), at the clip-relative tick. The chord
      // ops below run in that clip's tick space; the derived flat clip re-places
      // it at the same absolute tick the UI sees.
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/true);
      if (!target.valid) {
        consumeClipVersionForNoOp(runtime);
        return false;
      }
      const uint64_t relTick = target.relTick;
      const uint64_t placementAt = target.placementAt;
      diffTick = placementAt + relTick;
      daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
      clip.removeChordAt(relTick, column);
      clip.removeNoteAt(relTick, column);
      // A chord is a length-bearing event in the column, exactly like a note:
      // it ends whatever was sounding here, and its own length is stored, not
      // inferred at playback.
      if (daw::MusicalEvent* sounding = clip.soundingEventInColumn(relTick, column)) {
        daw::MusicalClip::truncateEventTo(*sounding, relTick);
      }
      if (duration == 0) {
        uint64_t spanEnd = loopEndNanotick.load(std::memory_order_acquire);
        if (spanEnd <= loopStartNanotick.load(std::memory_order_acquire)) {
          spanEnd = patternTicks;
        }
        // A chord entered past the current span still needs room to sound; reach
        // at least the end of its bar so writing past the end grows the song.
        spanEnd = std::max(spanEnd, barEndTick(nanotick));
        const uint64_t relSpanEnd =
            spanEnd > placementAt ? spanEnd - placementAt : 0;
        const auto next = clip.nextEventTickInColumn(relTick, column);
        const uint64_t end = next.value_or(relSpanEnd);
        duration = end > relTick ? end - relTick : 0;
      }
      event.nanotickOffset = relTick;
      event.payload.chord.durationNanoticks = duration;
      clip.addEvent(std::move(event));
      forkOwnedClip(*runtime, target.ownedIndex);
      growLengthsForContent(*runtime, target);
      snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    const uint32_t nextClipVersion = bumpClipVersionFor(runtime);
    daw::UiChordDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiChordDiffType::AddChord);
    diffPayload.trackId = trackId;
    diffPayload.clipVersion = nextClipVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(diffTick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((diffTick >> 32) & 0xffffffffu);
    diffPayload.durationLo = static_cast<uint32_t>(duration & 0xffffffffu);
    diffPayload.durationHi = static_cast<uint32_t>((duration >> 32) & 0xffffffffu);
    diffPayload.chordId = chordId;
    diffPayload.spreadNanoticks =
        (static_cast<uint32_t>(column) << 24) |
        (spreadNanoticks & 0x00ffffffu);
    diffPayload.packed = static_cast<uint32_t>(degree) |
                         (static_cast<uint32_t>(quality) << 8) |
                         (static_cast<uint32_t>(inversion) << 16) |
                         (static_cast<uint32_t>(baseOctave) << 24);
    diffPayload.flags = static_cast<uint16_t>(humanizeTiming & 0xffu) |
                        static_cast<uint16_t>((humanizeVelocity & 0xffu) << 8);
    emitChordDiff(diffPayload);
    return true;
}

bool applyRemoveNote(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint8_t pitch, uint16_t flags, bool recordUndo) {
  auto& clipDirty = deps.clipDirty;
  auto& clipVersion = deps.clipVersion;
  auto& commitStructuralEdit = deps.commitStructuralEdit;
  auto consumeClipVersionForNoOp = [&](auto&&... a) { return daw::engine::consumeClipVersionForNoOp(deps, decltype(a)(a)...); };
  auto& emitUiDiff = deps.emitUiDiff;
  auto forkOwnedClip = [&](auto&&... a) { return daw::engine::forkOwnedClip(deps, decltype(a)(a)...); };
  auto growLengthsForContent = [&](auto&&... a) { return daw::engine::growLengthsForContent(deps, decltype(a)(a)...); };
  auto& locateEditTarget = deps.locateEditTarget;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: RemoveNote failed - track " << trackId << " not found" << std::endl;
      return false;
    }

    std::optional<daw::ClipEditResult> result;
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      // A remove never creates a clip: a tick outside every clip is a no-op.
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/false);
      if (target.valid) {
        daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
        result = daw::removeNoteFromClip(clip, trackId, target.relTick, pitch,
                                         flags, runtime->trackClipVersion,
                                         recordUndo);
        if (result) {
          // HOW MANY APPEARANCES THIS EDIT REACHED. A clip is CONTENT and a placement is an
          // APPEARANCE, so an edit to a clip placed four times changes all four — that is the
          // Movement 3 promise ("fix the bass in chorus 1 and all three choruses change") and it
          // is also the most surprising thing in the program if nothing says it out loud. Two
          // placements of one clip draw as two identical rails; a note typed into one silently
          // rewrites the other, and there is no moment where the model announces itself.
          //
          // Counted HERE, where the answer is exact, rather than left to a client to infer by
          // grouping extents by clip id. On the event stream and in history.jsonl, so "this
          // changed 4 placements" is available to a console, to the linter, and to whoever reads
          // the journal a week later asking what happened.
          //
          // Counted AFTER forkOwnedClip: a copy-on-write fork repoints this track's placements,
          // so a count taken before it would be the pre-fork clip's.
          forkOwnedClip(*runtime, target.ownedIndex);
          if (target.ownedIndex < runtime->ownedClips.size()) {
            const uint32_t editedClipId = runtime->ownedClips[target.ownedIndex].id;
            uint32_t appearances = 0;
            for (const auto& pl : runtime->sourcePlacements) {
              if (pl.clipId == editedClipId) {
                ++appearances;
              }
            }
            if (appearances > 1) {
              DAW_EVENT("clip.shared_edit")
                  .field("track", trackId)
                  .field("clip", editedClipId)
                  .field("nanotick", nanotick)
                  .field("placements_affected", appearances);
            }
          }
          growLengthsForContent(*runtime, target);
          shiftDiffTick(result->diff, target.placementAt);
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
        }
      }
    }
    if (!result) {
      DAW_EVENT("clip.remove_note_missing")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("pitch", static_cast<uint32_t>(pitch))
          .field("action", "version_consumed");
      consumeClipVersionForNoOp(runtime);
      return false;
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipVersion.fetch_add(1, std::memory_order_acq_rel);  // see applyAddNote
    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);
    return true;
}

bool applyRemoveChord(ClipEditDeps& deps, uint32_t trackId, uint32_t chordId, bool recordUndo) {
  auto& commitStructuralEdit = deps.commitStructuralEdit;
  auto consumeClipVersionForNoOp = [&](auto&&... a) { return daw::engine::consumeClipVersionForNoOp(deps, decltype(a)(a)...); };
  auto forkOwnedClip = [&](auto&&... a) { return daw::engine::forkOwnedClip(deps, decltype(a)(a)...); };
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: RemoveChord failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    std::optional<daw::MusicalClip::RemovedChord> removed;
    std::shared_ptr<const ClipSnapshot> snapshot;
    uint64_t absTick = 0;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      // A chord id lives in exactly one owned clip; find and remove it there.
      for (size_t oi = 0; oi < runtime->ownedClips.size(); ++oi) {
        removed = runtime->ownedClips[oi].clip.removeChordById(chordId);
        if (removed) {
          absTick = firstPlacementAtForClip(*runtime, runtime->ownedClips[oi].id) +
                    removed->nanotick;
          forkOwnedClip(*runtime, oi);
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
          break;
        }
      }
    }
    if (!removed) {
      daw::LogLine() << "UI: RemoveChord - chord not found (track "
                << trackId << ", id " << chordId << ")" << std::endl;
      consumeClipVersionForNoOp(runtime);
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    return emitRemoveChordDiff(deps, trackId, *removed, absTick);
}

bool applyRemoveChordAt(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint8_t column, bool recordUndo) {
  auto& commitStructuralEdit = deps.commitStructuralEdit;
  auto consumeClipVersionForNoOp = [&](auto&&... a) { return daw::engine::consumeClipVersionForNoOp(deps, decltype(a)(a)...); };
  auto forkOwnedClip = [&](auto&&... a) { return daw::engine::forkOwnedClip(deps, decltype(a)(a)...); };
  auto& locateEditTarget = deps.locateEditTarget;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: RemoveChord failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    std::optional<daw::MusicalClip::RemovedChord> removed;
    std::shared_ptr<const ClipSnapshot> snapshot;
    uint64_t absTick = nanotick;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/false);
      if (target.valid) {
        removed = runtime->ownedClips[target.ownedIndex].clip.removeChordAt(
            target.relTick, column);
        if (removed) {
          absTick = target.placementAt + removed->nanotick;
          forkOwnedClip(*runtime, target.ownedIndex);
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
        }
      }
    }
    if (!removed) {
      daw::LogLine() << "UI: RemoveChord - chord not found (track "
                << trackId << ", tick " << nanotick
                << ", col " << static_cast<int>(column) << ")" << std::endl;
      consumeClipVersionForNoOp(runtime);
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    return emitRemoveChordDiff(deps, trackId, *removed, absTick);
}

bool requireMatchingClipVersion(ClipEditDeps& deps, uint32_t baseVersion, daw::UiCommandType commandType, uint32_t trackId) {
  auto& clipVersion = deps.clipVersion;
  auto& emitClipReject = deps.emitClipReject;
  auto& emitUiDiff = deps.emitUiDiff;
  auto& historyAppend = deps.historyAppend;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

    uint32_t current = clipVersion.load(std::memory_order_acquire);
    // Undo/Redo (and the other global-scope ops) can touch ANY track, so they are
    // gated on the global counter — comparing them against the caller's incidental
    // trackId would let an undo of a track-3 edit ride on track 0's version.
    if (!daw::uiCommandIsGlobalScope(commandType)) {
      bool haveTrack = false;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (trackId < tracks.size() && tracks[trackId] &&
            !tracks[trackId]->removed.load(std::memory_order_acquire)) {
          current = tracks[trackId]->trackClipVersion.load(std::memory_order_acquire);
          haveTrack = true;
        }
      }
      if (!haveTrack) {
        // A track-scoped edit naming a track that is not there used to fall through to
        // the global counter, get ACCEPTED, and then quietly do nothing when the edit
        // itself could not find the track. Refuse it here and say why: unlike a stale
        // base, retrying will never help, and the caller needs to know that.
        historyAppend(daw::uiCommandTypeName(commandType), "rejected:no_track", trackId,
                      baseVersion, "");
        DAW_EVENT("clip.unknown_track")
            .field("track", trackId)
            .field("command", static_cast<uint32_t>(commandType))
            .field("action", "rejected");
        emitClipReject(daw::UiClipRejectReason::UnknownTrack, trackId, baseVersion,
                       current, commandType);
        return false;
      }
    }
    daw::UiDiffPayload diffPayload{};
    if (daw::requireMatchingClipVersion(baseVersion, current, diffPayload)) {
      return true;
    }
    // Say WHICH track this version belongs to. The payload's clipVersion is now a
    // per-track counter (M2.17), and leaving trackId at its default 0 hands a client a
    // track-4 version labelled as track 0's — a trap that costs nothing to remove and
    // would be very hard to find. Global-scope commands keep 0, which is correct there:
    // they are gated on the global counter.
    const uint32_t scopeTrack =
        daw::uiCommandIsGlobalScope(commandType) ? 0u : trackId;
    diffPayload.trackId = scopeTrack;
    emitUiDiff(diffPayload);
    // Say it OUT LOUD. A resync request tells the caller to re-read; it does not tell
    // them they were refused, which edit, or what to retry with — so a client that
    // stamps the wrong base sees only "nothing happened", on every edit, forever.
    emitClipReject(daw::UiClipRejectReason::StaleBase, scopeTrack, baseVersion, current,
                   commandType);
    historyAppend(daw::uiCommandTypeName(commandType), "rejected:version", trackId,
                  baseVersion, "");
    DAW_EVENT("clip.version_mismatch")
        .field("base", baseVersion)
        .field("current", current)
        .field("command", static_cast<uint32_t>(commandType))
        .field("track", trackId)
        .field("action", "resync_requested");
    return false;
}

PlacementHit findPlacementAt(ClipEditDeps& deps, TrackRuntime& rt, uint64_t nanotick) {

    PlacementHit hit;
    for (auto& pl : rt.sourcePlacements) {
      if (!pl.at.has_value()) {
        continue;  // loose session cell: no timeline position
      }
      uint64_t len = pl.lengthNanoticks;
      if (len == 0) {
        for (const auto& c : rt.ownedClips) {
          if (c.id == pl.clipId) {
            len = c.lengthNanoticks;
            break;
          }
        }
      }
      if (nanotick < *pl.at || nanotick >= *pl.at + len) {
        continue;
      }
      ++hit.candidates;
      if (!hit.placement || *pl.at >= *hit.placement->at) {
        hit.placement = &pl;
        hit.end = *pl.at + len;
      }
    }
    return hit;
}

void forkOwnedClip(ClipEditDeps& deps, TrackRuntime& rt, size_t ownedIndex) {
  auto& nextClipId = deps.nextClipId;
  auto isEditableClip = [&](auto&&... a) { return daw::engine::isEditableClip(deps, decltype(a)(a)...); };


    if (ownedIndex >= rt.ownedClips.size()) {
      return;
    }
    const uint32_t oldId = rt.ownedClips[ownedIndex].id;
    if (isEditableClip(rt, oldId)) {
      return;
    }
    const uint32_t newId = nextClipId.fetch_add(1, std::memory_order_acq_rel);
    rt.ownedClips[ownedIndex].id = newId;
    for (auto& p : rt.sourcePlacements) {
      if (p.clipId == oldId) {
        p.clipId = newId;
      }
    }
    rt.editableClipIds.push_back(newId);
}

void growLengthsForContent(ClipEditDeps& deps, TrackRuntime& rt, const EditTarget& t) {



    if (t.ownedIndex >= rt.ownedClips.size()) {
      return;
    }
    const uint64_t contentEnd = clipContentEnd(rt.ownedClips[t.ownedIndex].clip);
    auto& clip = rt.ownedClips[t.ownedIndex];
    if (clip.lengthNanoticks > 0) {
      clip.lengthNanoticks = std::max(clip.lengthNanoticks, contentEnd);
    }
    if (t.placementIndex < rt.sourcePlacements.size()) {
      auto& pl = rt.sourcePlacements[t.placementIndex];
      if (pl.lengthNanoticks > 0) {
        pl.lengthNanoticks = std::max(pl.lengthNanoticks, contentEnd);
      }
    }
}

uint32_t bumpClipVersionFor(ClipEditDeps& deps, TrackRuntime* runtime) {
  auto& clipVersion = deps.clipVersion;


    // ORDER MATTERS, and it is the reverse of the obvious one. The publisher GATES on
    // the global ("has anything changed?") and PUBLISHES the per-track value. If the
    // global moved first, a publish landing between the two increments would latch the
    // new gate value while writing the OLD per-track version — and then return early
    // forever after, because the gate already matches. That track's published base
    // would be permanently one behind, so every client reading it would present a stale
    // base and have every edit rejected. Bump the value first, the gate second.
    const uint32_t trackNext =
        runtime ? runtime->trackClipVersion.fetch_add(1, std::memory_order_acq_rel) + 1
                : 0;
    const uint32_t globalNext = clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    return runtime ? trackNext : globalNext;
}

void bumpAllTrackClipVersions(ClipEditDeps& deps) {
  auto& clipVersion = deps.clipVersion;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;


    // Per-track values first, the global gate last — see bumpClipVersionFor. The window
    // is at its widest here: the global bump used to come before a tracksMutex
    // acquisition that the publisher takes on every iteration, so an entire all-tracks
    // rebuild could complete inside it and every track's published base would be stuck
    // one behind immediately after a project load.
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      for (auto& rt : tracks) {
        if (rt) {
          rt->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    }
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
}

void ensurePlacementIds(ClipEditDeps& deps, std::vector<daw::ProjectPlacement>& placements) {
  auto& nextPlacementId = deps.nextPlacementId;


    for (const auto& pl : placements) {
      uint32_t seen = nextPlacementId.load(std::memory_order_relaxed);
      while (pl.id >= seen &&
             !nextPlacementId.compare_exchange_weak(seen, pl.id + 1,
                                                    std::memory_order_relaxed)) {
      }
    }
    for (auto& pl : placements) {
      if (pl.id == 0) {
        pl.id = nextPlacementId.fetch_add(1, std::memory_order_relaxed);
      }
    }
}

bool editIsLocalScope(ClipEditDeps& deps, uint32_t trackId, uint64_t nanotick, uint16_t flags) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto findPlacementAt = [&](auto&&... a) { return daw::engine::findPlacementAt(deps, decltype(a)(a)...); };


    if ((flags & daw::kUiEditScopeLocal) != 0) {
      return true;
    }
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      return false;
    }
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    // THE CHOSEN placement's flag, not "any placement here has it set" — so the scope decision
    // and the target decision are the same decision about the same appearance.
    const PlacementHit hit = findPlacementAt(*runtime, nanotick);
    if (hit.candidates > 1) {
      DAW_EVENT("local_edit.ambiguous_tick")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("candidates", hit.candidates)
          .field("chose", hit.placement ? hit.placement->id : 0u)
          .field("rule", "latest_start");
    }
    return hit.placement != nullptr && hit.placement->localEdits;
}

bool isEditableClip(ClipEditDeps& deps, const TrackRuntime& rt, uint32_t id) {



    for (uint32_t e : rt.editableClipIds) {
      if (e == id) {
        return true;
      }
    }
    return false;
}

uint32_t bumpTrackClipVersion(ClipEditDeps& deps, uint32_t trackId) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto bumpClipVersionFor = [&](TrackRuntime* rt_) {
    return daw::engine::bumpClipVersionFor(deps, rt_);
  };


    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    return bumpClipVersionFor(runtime);
}

void consumeClipVersionForNoOp(ClipEditDeps& deps, TrackRuntime* runtime) {
  auto bumpClipVersionFor = [&](TrackRuntime* rt_) {
    return daw::engine::bumpClipVersionFor(deps, rt_);
  };


    bumpClipVersionFor(runtime);
}

}  // namespace daw::engine
