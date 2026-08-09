#include "engine_handle_ui_entry.h"

#include "apps/engine_command_mutates.h"

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

void handleUiEntry(HandleUiEntryDeps& deps, const daw::EventEntry& entry) {
  // Re-bind every dependency to the name the body already uses. This is what lets the
  // 1,623 lines below be the untouched original.
  auto& automationCommandDeps = deps.automationCommandDeps;
  auto& bulkStreams = deps.bulkStreams;
  auto& bulkTick = deps.bulkTick;
  auto& chainCommandDeps = deps.chainCommandDeps;
  auto& clipCommandDeps = deps.clipCommandDeps;
  auto& deviceCommandDeps = deps.deviceCommandDeps;
  auto& enqueuePreview = deps.enqueuePreview;
  auto& handleAssembledBulk = deps.handleAssembledBulk;
  auto& historyAppend = deps.historyAppend;
  auto& markerCommandDeps = deps.markerCommandDeps;
  auto& modlinkCommandDeps = deps.modlinkCommandDeps;
  auto& moduleCommandDeps = deps.moduleCommandDeps;
  auto& noteCommandDeps = deps.noteCommandDeps;
  auto& patcherCommandDeps = deps.patcherCommandDeps;
  auto& placementCommandDeps = deps.placementCommandDeps;
  auto& projectCommandDeps = deps.projectCommandDeps;
  auto& requestCommandDeps = deps.requestCommandDeps;
  auto& rowopsCommandDeps = deps.rowopsCommandDeps;
  auto& samplerCommandDeps = deps.samplerCommandDeps;
  auto& trackCommandDeps = deps.trackCommandDeps;
  auto& arrangeTimeCommandDeps = deps.arrangeTimeCommandDeps;
  auto& trackpropsCommandDeps = deps.trackpropsCommandDeps;
  auto& transportCommandDeps = deps.transportCommandDeps;
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

    // RECORD A VERSION WHEN THIS COMMAND FINISHES, whichever of the 48 branches it leaves by.
    //
    // RAII rather than a line at the end, because there is no end: every branch returns from
    // inside itself. A recording site that has to be repeated 48 times is a site that will be
    // missed once, and "missed once" is exactly how undo came to cover 15 commands of 70.
    //
    // Nothing is captured BEFORE the command. History holds versions, not before/after pairs: the
    // state to undo to is the version already at the cursor, so one capture per edit is enough
    // and the pair cannot disagree with itself.
    // VERSION 0 ARRIVES WITH THE FIRST EDIT, not with a load.
    //
    // seed() had exactly one caller, inside loadProjectFromPath. A bare boot — no --project and
    // no `do load` — therefore reached its first edit with an EMPTY history, and commit()'s empty
    // branch pushes the document it is handed as version 0. That document is the state AFTER the
    // edit, so the first edit of such a session was permanently un-undoable: there was no earlier
    // version to reach, and undo() returned nullptr forever after.
    //
    // Not fixed by seeding at startup: that only covers the boot path, and it would put version 0
    // at a moment the engine picked rather than at the state the user's first edit departs from.
    // Seeding HERE, before the command runs, is correct for every entry into the engine and costs
    // one capture per session.
    auto seedHistoryIfEmpty = [&deps] {
      if (deps.documentHistory != nullptr && deps.captureDocument &&
          deps.documentHistory->size() == 0) {
        // A FULL PLUGIN READ, not a dirty-flag one. There is no previous snapshot to carry
        // anything forward from, so onlyDirty would leave version 0 holding no plugin state and
        // the first undo would restore a song with every plugin left where the edit put it.
        deps.documentHistory->seed(
            deps.captureDocument(),
            deps.capturePluginState ? deps.capturePluginState({}, /*onlyDirty=*/false)
                                    : daw::engine::PluginStateSnapshot{});
        DAW_EVENT("undo.seeded").field("where", "first_edit");
      }
    };
    if (commandUndoPolicy(commandType) != UndoPolicy::None) {
      seedHistoryIfEmpty();
    }

    // GESTURE BRACKETING. A drag is one undo step; see kUiCmdFlagGestureBegin in event_payloads.h.
    //
    // ONLY FOR COMMAND TYPES WHOSE FLAGS WORD IS FREE — gestureFlagsApplyTo. The first version read
    // these bits from every command, and bit 15 is already kUiEditScopeLocal on note commands and
    // kUiPatcherFlagHasDeviceId on the patcher payloads (whose device id covers bits 0-14, so bit
    // 14 is taken there too). `do note --local` therefore read as GestureEnd.
    const bool gestureCapable = daw::gestureFlagsApplyTo(commandType);
    const bool gestureWantsBegin =
        gestureCapable && (header.flags & daw::kUiCmdFlagGestureBegin) != 0;
    const bool gestureWantsEnd =
        gestureCapable && (header.flags & daw::kUiCmdFlagGestureEnd) != 0;

    // A NON-GESTURE command force-closes an open gesture, so a UI that died mid-drag cannot wedge
    // every later edit into one step. END is NOT handled here — see closeGestureAtEnd below.
    if (deps.documentHistory != nullptr && !gestureWantsEnd && !gestureWantsBegin &&
        deps.documentHistory->gestureOpen() &&
        commandUndoPolicy(commandType) == UndoPolicy::Version) {
      deps.documentHistory->endGesture();
      // AND THE ABANDONED DRAG GETS ITS PLUGINS. Mid-gesture commands defer the cross-process
      // capture to the command carrying END; when the pointer never comes back up there is no
      // END, and that step would keep the plugin state from BEFORE the drag — a version whose
      // document and plugins never coexisted. Paid once, here, at the moment the drag is
      // declared over, which is also the only moment we know it is.
      if (deps.capturePluginState) {
        deps.documentHistory->amendPluginState(deps.capturePluginState(
            deps.documentHistory->pluginStateAtCursor(), /*onlyDirty=*/true));
      }
      DAW_EVENT("undo.gesture_force_closed")
          .field("by", std::string(daw::uiCommandTypeName(commandType)));
    }

    struct RecordVersion {
      HandleUiEntryDeps& d;
      UndoPolicy policy;
      const char* label;
      // SAMPLED AT CONSTRUCTION, NOT READ AT DESTRUCTION — and the difference is a bug I wrote and
      // caught by tracing it. beginGesture() runs after this guard is built but before it fires, so
      // reading gestureOpen() in the destructor would make the drag's FIRST command amend instead
      // of commit, leaving an interrupted drag with no step to undo to.
      bool amendForGesture;
      // MID-DRAG, SKIP THE PLUGINS. capturePluginState is a blocking round trip to another
      // process per hosted plugin, and a knob drag emits one command per milli-unit — a thousand
      // of them per drag would make the engine unusable, which is exactly why the ruling put
      // gesture coalescing ahead of this. So a command inside an open gesture that is not the
      // one carrying END carries the PREVIOUS snapshot forward unread, and the END command pays
      // for the whole drag once. An abandoned drag (no END ever arrives) is caught by the
      // force-close path, which amends the step's plugin state then.
      bool deferPluginCapture;
      PluginStateSnapshot capture() const {
        if (!d.capturePluginState || d.documentHistory == nullptr) {
          return {};
        }
        const PluginStateSnapshot previous = d.documentHistory->pluginStateAtCursor();
        if (deferPluginCapture) {
          return previous;
        }
        return d.capturePluginState(previous, /*onlyDirty=*/true);
      }
      ~RecordVersion() {
        if (policy == UndoPolicy::None || d.documentHistory == nullptr || !d.captureDocument) {
          return;
        }
        // AN AUDITION KEEPS THE HISTORY HONEST WITHOUT ADDING TO IT. See commandUndoPolicy: the
        // swap must not consume an undo step, but the cursor's version still has to follow the
        // live document or the next undo flips the placement back as collateral.
        // MID-GESTURE COMMANDS AMEND. The drag's first command opened the step; every one after
        // it rewrites that step instead of adding another, so a 1000-command knob drag is ONE undo
        // step holding the final value. Inert until a client sets the flags.
        if (policy == UndoPolicy::Version && amendForGesture) {
          d.documentHistory->amend(d.captureDocument(), capture());
          return;
        }
        if (policy == UndoPolicy::Amend) {
          d.documentHistory->amend(d.captureDocument(), capture());
          DAW_EVENT("undo.version_amended")
              .field("label", std::string(label))
              .field("cursor", static_cast<uint64_t>(d.documentHistory->cursor()));
          return;
        }
        if (!d.documentHistory->commit(d.captureDocument(), label, capture())) {
          // THE COMMAND CHANGED NOTHING — refused, or applied a value already in place. Recorded
          // as its own event rather than silently: "no version appeared" and "the guard never
          // ran" look identical from outside, and only one of them is correct behaviour.
          DAW_EVENT("undo.version_unchanged")
              .field("label", std::string(label))
              .field("versions", static_cast<uint64_t>(d.documentHistory->size()))
              .field("cursor", static_cast<uint64_t>(d.documentHistory->cursor()));
          return;
        }
        // OBSERVABLE FROM OUTSIDE, because "the history is being built" is otherwise a claim
        // nobody can check until undo is repointed at it. A check can read this line and assert
        // the count rises once per mutating command and not at all for a query.
        DAW_EVENT("undo.version_recorded")
            .field("label", std::string(label))
            .field("versions", static_cast<uint64_t>(d.documentHistory->size()))
            .field("cursor", static_cast<uint64_t>(d.documentHistory->cursor()))
            .field("bytes", static_cast<uint64_t>(d.documentHistory->bytes()));
      }
    } recordVersion{deps, commandUndoPolicy(commandType),
                    daw::engine::commandLabel(commandType),
                    deps.documentHistory != nullptr &&
                        deps.documentHistory->gestureAmendable(),
                    // Sampled here for the same reason as amendForGesture: beginGesture() runs
                    // below, so reading it in the destructor would defer the capture of the
                    // drag's FIRST command too — and a one-command drag would then hold no
                    // plugin state at all.
                    deps.documentHistory != nullptr &&
                        deps.documentHistory->gestureOpen() && !gestureWantsEnd};

    // Opened AFTER the guard is built, so THIS command still commits and the drag has a step to
    // undo to even if the pointer never comes back up.
    //
    // BEGIN|END TOGETHER is a click with no travel, and must NOT leave a gesture open: the next
    // drag's BEGIN would then be swallowed by the force-close guard and its first command would
    // amend the PREVIOUS undo step instead of committing its own.
    if (deps.documentHistory != nullptr && gestureWantsBegin && !gestureWantsEnd) {
      deps.documentHistory->beginGesture();
    }

    // CLOSES AFTER THE GUARD HAS SAMPLED, which is the whole point of it being here rather than
    // above. Closing END early made amendForGesture read false, so the drag's FINAL command took
    // the commit() path and pushed a SECOND version — one drag, two undo steps, and the first
    // Ctrl-Z landing on the second-to-last drag sample, a value the user never chose. That is the
    // exact mirror of the BEGIN bug fixed by sampling at construction, written into the same
    // commit under a comment claiming the opposite. Found by review.
    //
    // RAII so it runs on every return path, including the bulk branch's early return.
    struct CloseGestureAtEnd {
      daw::engine::DocumentHistory* history;
      bool closing;
      ~CloseGestureAtEnd() {
        if (closing && history != nullptr) {
          history->endGesture();
        }
      }
    } closeGestureAtEnd{deps.documentHistory,
                        deps.documentHistory != nullptr && gestureWantsEnd};
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
        // THE ENVELOPE IS NOT THE COMMAND. BulkChunk itself changes nothing, so the guard above
        // was built with mutates=false — correct for the carrier, wrong for what it carries.
        // SetClipText, SamplerSetSlotName and SamplerSetEnvelopePoints arrive ONLY this way and
        // can never arrive any other way, so with the guard reading the OUTER opcode they were
        // classified `true` in commandMutatesDocument and still recorded nothing: draw an
        // envelope, undo, and it is gone with no version to redo from. Re-point the guard at the
        // opcode the envelope spells, which is its first two bytes and is what
        // handleAssembledBulk itself dispatches on.
        if (assembled.size() >= sizeof(uint16_t)) {
          uint16_t innerOp = 0;
          std::memcpy(&innerOp, assembled.data(), sizeof(innerOp));
          const auto innerType = static_cast<daw::UiCommandType>(innerOp);
          recordVersion.policy = commandUndoPolicy(innerType);
          recordVersion.label = daw::engine::commandLabel(innerType);
          // The seed above ran on the OUTER opcode, which is non-mutating, so it declined. An
          // envelope drawn as the first act of a bare session would otherwise land as version 0.
          if (recordVersion.policy != UndoPolicy::None) {
            seedHistoryIfEmpty();
          }
        }
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
    // 369 lines of body now live in apps/engine_arrangetime_commands.cpp, moved verbatim.
    if (entry.size == sizeof(daw::UiArrangeTimeCommandPayload) &&
        (commandType == daw::UiCommandType::SetTimeSignature ||
         commandType == daw::UiCommandType::InsertRemoveTime)) {
      daw::engine::handleArrangeTime(arrangeTimeCommandDeps, entry, commandType);
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
      daw::engine::handleForkSwapPlacementClip(placementCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetPlacementEditScope)) {
      daw::engine::handleSetPlacementEditScope(placementCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RevertPlacementOverrides)) {
      daw::engine::handleRevertPlacementOverrides(placementCommandDeps, entry, payload);
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
      daw::engine::handleAddTrack(trackCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RemoveTrack)) {
      daw::engine::handleRemoveTrack(trackCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::MovePlacement)) {
      daw::engine::handleMovePlacement(placementCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RemovePlacement)) {
      daw::engine::handleRemovePlacement(placementCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::ResizePlacement)) {
      daw::engine::handleResizePlacement(placementCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::AddPlacement)) {
      daw::engine::handleAddPlacement(placementCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::TogglePlay)) {
      daw::engine::handleTogglePlay(transportCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Stop)) {
      daw::engine::handleStop(transportCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Panic)) {
      daw::engine::handlePanic(transportCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetPosition)) {
      daw::engine::handleSetPosition(transportCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestChainSnapshot)) {
      daw::engine::handleRequestChainSnapshot(requestCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Quit)) {
      daw::engine::handleQuit(transportCommandDeps, entry, payload);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTempo)) {
      daw::engine::handleSetTempo(transportCommandDeps, entry, payload);
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
      daw::engine::handleSetLoopRange(transportCommandDeps, entry, payload);
    } else {
      // AN OPCODE THAT FALLS OFF THE END OF THIS CHAIN DID NOTHING, AND SAID NOTHING.
      //
      // Dispatch here is 34 size-gated blocks — each of which RETURNS — followed by this one
      // else-if chain. A command with an enum entry, a name, a CLI verb and no arm therefore
      // reached the bottom, fell out of the chain, and returned normally. The journal still
      // recorded it as accepted, because the journal is written by the command thread from the
      // fact that a command ARRIVED, not from anything the dispatch did with it. So the write
      // path reported success end to end and the state never moved.
      //
      // That is the most expensive failure shape on this wire and it is already documented as
      // such: a wrong payload's default failure mode is a SILENT no-op, which is why the engine
      // logs *_rejected events everywhere else. This was the one place that could swallow a whole
      // command without a word.
      //
      // A switch on UiCommandType with -Werror=switch would make it a COMPILE error instead —
      // strictly better, and worth doing when this chain is finally emptied into the command
      // modules it already has. Until then, loud at runtime beats silent.
      DAW_EVENT("ui.op_unhandled")
          .field("op", daw::uiCommandTypeName(commandType))
          .field("code", static_cast<uint32_t>(payload.commandType))
          .field("size", static_cast<uint32_t>(entry.size));
      daw::LogLine() << "UI: opcode " << static_cast<uint32_t>(payload.commandType) << " ("
                     << daw::uiCommandTypeName(commandType)
                     << ") reached the end of the dispatch chain with no handler — the command "
                        "was accepted and journalled, and did nothing." << std::endl;
    }
}

}  // namespace daw::engine
