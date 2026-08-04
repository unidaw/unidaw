#include "engine_ui_thread.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "event_log.h"

namespace daw::engine {
namespace {

// CAME WITH ITS ONLY CALLERS. This was a file-scope function in daw_engine_main.cpp and every one
// of its three call sites was inside this thread's body — so it is private here rather than shared
// through a header. A free function is invisible to capture enumeration, the same way a constant
// is: it surfaced when the new translation unit failed to compile, not when the captures were
// listed.
bool uiDebugEnabled() {
  static const bool enabled = []() {
    const char* env = std::getenv("DAW_UI_DEBUG");
    return env && std::string(env) == "1";
  }();
  return enabled;
}

}  // namespace

void runUiThread(UiThreadDeps& deps) {
  auto& running = deps.running;
  auto& getRingUi = deps.getRingUi;
  auto& getRingUiAgent = deps.getRingUiAgent;
  auto& getRingUiEdit = deps.getRingUiEdit;
  auto& handleUiEntry = deps.handleUiEntry;
  auto& uiDiffNowMs = deps.uiDiffNowMs;


    daw::LogLine() << "UI: command thread started" << std::endl;
    uint64_t lastIdleLogMs = 0;
    // M2.18: abandoned-slot recovery for the multi-producer rings. A producer reserves
    // a slot, then fills and publishes it — a few instructions apart. If it dies in
    // between (Ctrl-C'd daw-cli, crashed UI) the slot never becomes ready and the
    // consumer would wait at it forever, wedging every later command.
    //
    // The threshold is deliberately long. A slot that is merely slow belongs to a
    // producer that is descheduled or page-faulting, and retiring it while that
    // producer is still alive lets it publish into a slot someone else has since
    // claimed. Two seconds is far beyond any scheduling hiccup and far below any
    // useful patience for a wedged ring.
    constexpr uint64_t kStalledSlotGraceMs = 2000;
    struct StallWatch { uint32_t slot = 0; uint64_t sinceMs = 0; bool active = false; };
    StallWatch stallUi, stallAgent;
    auto recoverStalledRing = [&](daw::EventRingView& ring, StallWatch& watch,
                                  const char* which) {
      uint32_t slot = 0;
      if (!daw::ringStalledSlot(ring, slot)) {
        watch.active = false;
        return;
      }
      const uint64_t nowMs = uiDiffNowMs();
      if (!watch.active || watch.slot != slot) {
        watch = StallWatch{slot, nowMs, true};
        return;
      }
      if (nowMs - watch.sinceMs < kStalledSlotGraceMs) {
        return;
      }
      DAW_EVENT("ring.abandoned_slot")
          .field("ring", which)
          .field("slot", slot)
          .field("waited_ms", static_cast<uint32_t>(nowMs - watch.sinceMs))
          .field("action", "retired");
      daw::LogLine() << "UI: retiring abandoned " << which << " ring slot " << slot
                << " (producer reserved it and never published; it probably died)"
                << std::endl;
      daw::ringSkipStalledSlot(ring);
      watch.active = false;
    };
    while (running.load()) {
      auto ringUi = getRingUi();
      auto ringUiEdit = getRingUiEdit();
      auto ringUiAgent = getRingUiAgent();
      if (ringUi.mask == 0 && ringUiEdit.mask == 0 && ringUiAgent.mask == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      daw::EventEntry uiEntry;
      daw::UiEditBatchEntry editBatch{};
      bool handled = false;
      while (daw::uiEditRingPop(ringUiEdit, editBatch)) {
        const uint32_t opCount =
            std::min<uint32_t>(editBatch.opCount, daw::kUiEditBatchMaxOps);
        if (opCount != editBatch.opCount) {
          // Only reachable from a malformed or mismatched producer.
          DAW_EVENT("edit_ring.op_count_clamped")
              .field("batch", editBatch.batchId)
              .field("claimed", editBatch.opCount)
              .field("applied", opCount);
        } else if (uiDebugEnabled()) {
          DAW_EVENT("edit_ring.batch")
              .field("batch", editBatch.batchId)
              .field("ops", opCount);
        }
        for (uint32_t i = 0; i < opCount; ++i) {
          handleUiEntry(editBatch.ops[i]);
        }
        handled = true;
      }
      while (daw::ringPop(ringUi, uiEntry)) {
        if (uiDebugEnabled()) {
          daw::LogLine() << "UI: received command entry size "
                    << uiEntry.size << " type " << uiEntry.type << std::endl;
        }
        handleUiEntry(uiEntry);
        handled = true;
      }
      // The agent's own ring, drained through the same handler so an agent edit
      // is indistinguishable from a UI edit once inside the engine.
      while (daw::ringPop(ringUiAgent, uiEntry)) {
        handleUiEntry(uiEntry);
        handled = true;
      }
      recoverStalledRing(ringUi, stallUi, "ui");
      recoverStalledRing(ringUiAgent, stallAgent, "agent");
      if (!handled) {
        const uint64_t nowMs = uiDiffNowMs();
        if (uiDebugEnabled() && nowMs - lastIdleLogMs >= 1000) {
          lastIdleLogMs = nowMs;
          const uint32_t read =
              ringUi.header ? ringUi.header->readIndex.load(std::memory_order_relaxed) : 0;
          const uint32_t write =
              ringUi.header ? ringUi.header->writeIndex.load(std::memory_order_relaxed) : 0;
          daw::LogLine() << "UI: command ring idle (read " << read
                    << ", write " << write << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    daw::LogLine() << "UI: command thread exiting" << std::endl;
}

}  // namespace daw::engine
