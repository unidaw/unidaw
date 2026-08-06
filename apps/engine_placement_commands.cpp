#include "engine_placement_commands.h"

#include <cstring>

#include "event_log.h"

namespace daw::engine {

// Resize's "leave this field alone" sentinel. It lived at file scope in
// engine_handle_ui_entry.cpp and came here with its only remaining users — a constant is
// never reported as a missing capture, so this one surfaced only when the new translation
// unit failed to compile, which is the documented way constants show up in these moves.
constexpr uint64_t kPlacementUnchanged = 0xFFFFFFFFFFFFFFFFull;


void handleForkSwapPlacementClip(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& clipDirty = deps.clipDirty;
  auto& historyAppend = deps.historyAppend;
  auto& nextClipId = deps.nextClipId;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& recomputeSongEnd = deps.recomputeSongEnd;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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
}

void handleSetPlacementEditScope(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& clipDirty = deps.clipDirty;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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
}

void handleRevertPlacementOverrides(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& clipDirty = deps.clipDirty;
  auto& pushStructuralUndo = deps.pushStructuralUndo;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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
}

void handleMovePlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& applyPlacementEdit = deps.applyPlacementEdit;
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& clipDirty = deps.clipDirty;
  auto& nextClipId = deps.nextClipId;
  auto& pushUndo = deps.pushUndo;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& snapshotTrackStore = deps.snapshotTrackStore;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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
          // AND THE SONG END, which this branch did everything else itself and forgot.
          //
          // The same-track path reaches this through applyPlacementEdit; the cross-track branch
          // relocates the placement, rebuilds both flat clips, both audio renders, the undo entry
          // and both clip versions by hand — and never recomputed the extent. So dragging the
          // placement that DEFINES the song's end into another lane left the end where it was:
          // with loopUserSet false the transport keeps looping the old span, the clip that was
          // just moved never sounds, and the loop bracket does not move. Visible in exactly the
          // "arrange and piano roll are one data model" gesture.
          //
          // Safe here: the two-track lock scope has closed above, so there is no re-entrancy.
          deps.recomputeSongEnd();
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
}

void handleAddPlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& applyPlacementEdit = deps.applyPlacementEdit;
  auto& nextPlacementId = deps.nextPlacementId;

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
}

void handleResizePlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& applyPlacementEdit = deps.applyPlacementEdit;

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
}

void handleRemovePlacement(PlacementCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& applyPlacementEdit = deps.applyPlacementEdit;

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
}

}  // namespace daw::engine
