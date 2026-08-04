#include "engine_harmony_timeline.h"

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

}  // namespace daw::engine
