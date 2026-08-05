#include "engine_transport_commands.h"

#include <cstring>
#include <iostream>

#include "engine_rt_helpers.h"
#include "event_log.h"

namespace daw::engine {

void handleSetLoopRange(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& loopEndNanotick = deps.engineState.transport.loopEndNanotick;
  auto& loopStartNanotick = deps.engineState.transport.loopStartNanotick;
  auto& loopUserSet = deps.engineState.transport.loopUserSet;
  auto& transportNanotick = deps.engineState.transport.transportNanotick;

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

void handlePanic(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& heldPreview = deps.engineState.previewQueue.heldPreview;
  auto& masterTrack = deps.masterTrack;
  auto& panicPending = deps.panicPending;
  auto& pendingPreviewNotes = deps.engineState.previewQueue.pendingPreviewNotes;
  auto& playing = deps.engineState.transport.playing;
  auto& previewMutex = deps.engineState.previewQueue.previewMutex;
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;

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
}

void handleSetTempo(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& loadedTempoMap = deps.engineState.songTiming.loadedTempoMap;
  auto& tempoProvider = deps.tempoProvider;

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
}

void handleStop(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& heldPreview = deps.engineState.previewQueue.heldPreview;
  auto& pendingPreviewNotes = deps.engineState.previewQueue.pendingPreviewNotes;
  auto& playing = deps.engineState.transport.playing;
  auto& previewMutex = deps.engineState.previewQueue.previewMutex;
  auto& resetTimeline = deps.resetTimeline;

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
}

void handleSetPosition(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& loopEndNanotick = deps.engineState.transport.loopEndNanotick;
  auto& loopStartNanotick = deps.engineState.transport.loopStartNanotick;
  auto& patternTicks = deps.patternTicks;
  auto& transportElapsedNanotick = deps.engineState.transport.transportElapsedNanotick;
  auto& transportNanotick = deps.engineState.transport.transportNanotick;

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
}

void handleQuit(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& playing = deps.engineState.transport.playing;
  auto& restartCv = deps.restartCv;
  auto& running = deps.running;

      // The last UI went away. Silence first, then exit: `running` unwinds through
      // the join/stop path at the bottom of main(), which takes a moment, and a
      // moment of audio after the window closed is exactly what this exists to
      // stop. The sidecar only sends this after a grace period, so a page reload
      // does not end the session.
      playing.store(false, std::memory_order_release);
      std::cout << "UI: last client gone — engine shutting down" << std::endl;
      running.store(false, std::memory_order_release);
      restartCv.notify_all();
}

void handleTogglePlay(TransportCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& playing = deps.engineState.transport.playing;

      const bool next = !playing.load(std::memory_order_acquire);
      playing.store(next, std::memory_order_release);
      std::cout << "UI: Transport " << (next ? "Play" : "Pause") << std::endl;
}

}  // namespace daw::engine
