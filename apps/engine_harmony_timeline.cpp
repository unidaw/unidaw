#include "engine_harmony_timeline.h"
#include "engine_ui_publish.h"  // currentCommandId — the dispatch id

#include "engine_rt_helpers.h"
#include "event_log.h"

namespace daw::engine {

std::optional<daw::HarmonyEvent> HarmonyTimeline::getHarmonyAt(uint64_t nanotick) {

    std::lock_guard<std::mutex> lock(harmonyMutex);
    return daw::engine::harmonyAtOrDefault(harmonyEvents, nanotick);
}

const daw::Scale* HarmonyTimeline::getScaleForHarmony(const daw::HarmonyEvent& harmony) {

    return scaleRegistry.find(harmony.scaleId);
}

bool HarmonyTimeline::addOrUpdateHarmony(uint64_t nanotick, uint32_t root, uint32_t scaleId, bool recordUndo) {

    bool updated = false;
    daw::HarmonyEvent previous{};
    {
      std::lock_guard<std::mutex> lock(harmonyMutex);
      auto it = std::lower_bound(
          harmonyEvents.begin(), harmonyEvents.end(), nanotick,
          [](const daw::HarmonyEvent& lhs, uint64_t tick) {
            return lhs.nanotick < tick;
          });
      if (it != harmonyEvents.end() && it->nanotick == nanotick) {
        previous = *it;
        it->root = root;
        it->scaleId = scaleId;
        updated = true;
      } else {
        harmonyEvents.insert(it, daw::HarmonyEvent{nanotick, root, scaleId, 0});
      }
    }
    if (recordUndo) {
      daw::UndoEntry undo{};
      undo.nanotick = nanotick;
      if (updated) {
        undo.type = daw::UndoType::UpdateHarmony;
        undo.harmonyRoot = previous.root;
        undo.harmonyScaleId = previous.scaleId;
        undo.harmonyRoot2 = root;
        undo.harmonyScaleId2 = scaleId;
      } else {
        undo.type = daw::UndoType::RemoveHarmony;
        undo.harmonyRoot = root;
        undo.harmonyScaleId = scaleId;
      }
      pushHarmonyUndo(undo);
    }
    harmonyDirty.store(true, std::memory_order_release);
    const uint32_t nextVersion =
        harmonyVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiHarmonyDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(
        updated ? daw::UiHarmonyDiffType::UpdateEvent
                : daw::UiHarmonyDiffType::AddEvent);
    diffPayload.harmonyVersion = nextVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
    diffPayload.root = root;
    diffPayload.scaleId = scaleId;
    emitHarmonyDiff(diffPayload);
    return true;
}

bool HarmonyTimeline::removeHarmony(uint64_t nanotick, bool recordUndo) {

    bool removed = false;
    daw::HarmonyEvent removedEvent{};
    {
      std::lock_guard<std::mutex> lock(harmonyMutex);
      auto it = std::lower_bound(
          harmonyEvents.begin(), harmonyEvents.end(), nanotick,
          [](const daw::HarmonyEvent& lhs, uint64_t tick) {
            return lhs.nanotick < tick;
          });
      if (it != harmonyEvents.end() && it->nanotick == nanotick) {
        removedEvent = *it;
        harmonyEvents.erase(it);
        removed = true;
      }
    }
    if (!removed) {
      return false;
    }
    if (recordUndo) {
      daw::UndoEntry undo{};
      undo.type = daw::UndoType::AddHarmony;
      undo.nanotick = nanotick;
      undo.harmonyRoot = removedEvent.root;
      undo.harmonyScaleId = removedEvent.scaleId;
      pushHarmonyUndo(undo);
    }
    harmonyDirty.store(true, std::memory_order_release);
    const uint32_t nextVersion =
        harmonyVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiHarmonyDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiHarmonyDiffType::RemoveEvent);
    diffPayload.harmonyVersion = nextVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
    emitHarmonyDiff(diffPayload);
    return true;
}

bool HarmonyTimeline::requireMatchingHarmonyVersion(uint32_t baseVersion,
                                                    daw::UiCommandType commandType) {

    const uint32_t current = harmonyVersion.load(std::memory_order_acquire);
    if (baseVersion == current) {
      return true;
    }
    daw::UiHarmonyDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiHarmonyDiffType::ResyncNeeded);
    diffPayload.harmonyVersion = current;
    // P2-CMD-00 / AE-P1.2 item 27, second of the three refusal channels. The payload has carried
    // the id since step 1 and nothing wrote it, so a caller could see that SOME harmony write was
    // refused and not whether it was ITS OWN — `commandType` names a kind, not an instance.
    //
    // The two success paths above (Add/Update/Remove) deliberately do not set it: they are
    // notifications of something that HAPPENED, not answers to a command, and an id there would
    // invite a reader to correlate a broadcast with its own request.
    const uint64_t commandId = daw::engine::currentCommandId();
    diffPayload.correlationLo = static_cast<uint32_t>(commandId & 0xFFFFFFFFu);
    diffPayload.correlationHi = static_cast<uint32_t>(commandId >> 32);
    emitHarmonyDiff(diffPayload);
    DAW_EVENT("harmony.version_mismatch")
        .field("base", baseVersion)
        .field("current", current)
        .field("command", static_cast<uint32_t>(commandType))
        .field("action", "resync_requested");
    return false;
}

}  // namespace daw::engine
