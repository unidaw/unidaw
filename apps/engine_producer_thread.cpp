#include "engine_producer_thread.h"

#include <iostream>

#include "engine_rt_helpers.h"
#include "rt_thread.h"
#include "event_log.h"

namespace daw::engine {

void runProducerThread(ProducerThreadDeps& deps) {
  auto& engineState = deps.engineState;
  auto& audioPlaybackBlockId = deps.audioPlaybackBlockId;
  auto& engineConfig = deps.engineConfig;
  auto& getHarmonyAt = deps.getHarmonyAt;
  auto& getRingCtrl = deps.getRingCtrl;
  auto& getRingStd = deps.getRingStd;
  auto& getScaleForHarmony = deps.getScaleForHarmony;
  auto& harmonyTimeline = deps.harmonyTimeline;
  auto& lastOverflowTick = deps.lastOverflowTick;
  auto& nextBlockId = deps.nextBlockId;
  auto& nextNoteId = deps.nextNoteId;
  auto& offlineProducerArmed = deps.offlineProducerArmed;
  auto& offlineRender = deps.offlineRender;
  auto& panicPending = deps.panicPending;
  auto& patcherParallel = deps.patcherParallel;
  auto& patcherPool = deps.patcherPool;
  auto& patternTicks = deps.patternTicks;
  auto& projectSeed = deps.projectSeed;
  auto& publishedCallback = deps.publishedCallback;
  auto& quantizePitch = deps.quantizePitch;
  auto& resetTimeline = deps.resetTimeline;
  auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
  auto& running = deps.running;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& tempoProvider = deps.tempoProvider;
  auto& testThrottleMs = deps.testThrottleMs;
  auto& tickConverter = deps.tickConverter;
  auto& traceNotes = deps.traceNotes;
  auto& transport = deps.engineState.transport;
  auto& warnedEventOutsideBlock = deps.warnedEventOutsideBlock;
  auto& writeMirrorParams = deps.writeMirrorParams;


    // The producer renders/dispatches each block ahead of the device and paces to it;
    // any preemption here directly starves the ring. Raise it above background/UI work.
    daw::elevateToAudioPriority();
    // DENORMALS FLUSH TO ZERO on the producer, which is where the sampler renders. Set once per
    // thread rather than per block: it is a control-register write, and doing it in the render
    // loop would cost more than the denormals it prevents.
    daw::enableFlushToZero();
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);
    // How long a block LASTS. Producing one must cost less than this or the producer can never
    // catch up. This is the budget the load counters are measured against.
    const uint64_t producerBlockBudgetUs =
        engineConfig.sampleRate > 0.0
            ? static_cast<uint64_t>(static_cast<double>(engineConfig.blockSize) /
                                    engineConfig.sampleRate * 1e6)
            : 0;
    const bool debugStall = std::getenv("DAW_ENGINE_DEBUG_STALL") != nullptr;
    const auto stallStart = std::chrono::steady_clock::now();
    uint64_t stallLogMs = 0;
    uint32_t lastPlaybackBlock = 0;
    std::string stallHosts;
    auto lastPlaybackAdvance = std::chrono::steady_clock::now();
    const auto playbackStallLimit = std::chrono::milliseconds(100);
    auto stallNowMs = [&]() -> uint64_t {
      return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - stallStart)
              .count());
    };
    auto logStall = [&](const char* reason,
                        uint32_t nextId,
                        uint32_t minCompleted,
                        uint32_t currentPlayback,
                        uint32_t extra) {
      if (!debugStall) {
        return;
      }
      const uint64_t nowMs = stallNowMs();
      if (nowMs - stallLogMs < 500) {
        return;
      }
      stallLogMs = nowMs;
      daw::LogLine() << "Engine: producer stall (" << reason
                << ") next=" << nextId
                << " minCompleted=" << minCompleted
                << " playback=" << currentPlayback
                << " extra=" << extra
                << " hosts=[" << stallHosts << "]" << std::endl;
    };
    // THE TRANSPORT ADVANCES BY A CARRIED FRACTION, not by a rounded tick.
    //
    // This used to round to a whole nanotick per block. A block is not a whole number of ticks —
    // at 120 bpm / 44.1 kHz a 256-frame block is 11145.898 and a 64-frame block is 2786.48 — so
    // rounding once per block accumulated error, at a rate that DEPENDED ON THE BLOCK SIZE. The
    // tick position slid against the sample counter by about 1.3 samples per 7000 frames at 64
    // frames, and a note's frame is blockSampleStart + an offset measured from the tick. That is
    // why the same project rendered at 64 and at 256 frames diverged (task #84), and why
    // rewriting the note OFFSET could never fix it: both formulations measured from a drifting
    // base.
    //
    // Carrying the remainder bounds the error below one nanotick forever instead of letting it
    // grow. blockTicksFor is called EXACTLY ONCE per block — advanceTransport runs only on the
    // no-host path, which then continues — so the carry advances once per block, which is the
    // whole reason this can be stateful at all.
    long double tickCarry = 0.0L;
    auto blockTicksFor = [&](uint64_t atNanotick) -> uint64_t {
      tickCarry += tickConverter.samplesToNanoticksExact(
          static_cast<int64_t>(engineConfig.blockSize), atNanotick);
      const long double whole = std::floor(tickCarry);
      tickCarry -= whole;
      return static_cast<uint64_t>(whole);
    };
      // Built once per producer thread, immediately before the loop: every member is a
      // reference to something that outlives it, so the per-block call adds no work.
      daw::engine::ProducerBlockDeps producerBlockDeps{
      engineState, blockDuration, blockTicksFor, debugStall,
      engineConfig, getHarmonyAt, getRingCtrl, getRingStd, getScaleForHarmony, harmonyTimeline,
      lastOverflowTick, nextBlockId, nextNoteId,
      offlineRender, panicPending, patcherParallel, patcherPool,
      producerBlockBudgetUs, projectSeed,
      publishedCallback, quantizePitch, resolveDevicePluginPath, tempoProvider,
      tickConverter, traceNotes, patternTicks, warnedEventOutsideBlock, writeMirrorParams
  };

    // Held across iterations so collecting host progress costs no allocation on the RT path.
    std::vector<daw::engine::HostProgress> hostProgress;
    hostProgress.reserve(daw::kUiMaxTracks);

    while (running.load()) {
      // Offline: produce nothing until the pump says the transport is at a known start. See
      // offlineProducerArmed.
      if (offlineRender && !offlineProducerArmed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (testThrottleMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(testThrottleMs));
      }
      auto trackSnapshot = snapshotTracks();

      if (trackSnapshot.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      const bool isPlaying = transport.playing.load(std::memory_order_acquire);
      auto advanceTransport = [&]() {
        const auto loop = daw::engine::effectiveLoop(
            transport.loopStartNanotick.load(std::memory_order_acquire),
            transport.loopEndNanotick.load(std::memory_order_acquire), patternTicks);
        const uint64_t loopStartTicks = loop.startTick;
        const uint64_t loopEndTicks = loop.endTick;
        const uint64_t currentTicks =
            transport.transportNanotick.load(std::memory_order_acquire);
        const uint64_t blockTicks = blockTicksFor(currentTicks);
        uint64_t nextTicks = currentTicks + blockTicks;
        nextTicks = daw::engine::advanceTransportTick(nextTicks, loopStartTicks, loopEndTicks);
        transport.transportNanotick.store(nextTicks, std::memory_order_release);
        transport.transportElapsedNanotick.fetch_add(blockTicks, std::memory_order_acq_rel);
      };
      bool anyReady = false;
      for (auto* runtime : trackSnapshot) {
        if (runtime->hostReady.load(std::memory_order_acquire)) {
          anyReady = true;
          break;
        }
      }
      if (!anyReady) {
        if (isPlaying) {
          advanceTransport();
        }
        std::this_thread::sleep_for(blockDuration);
        continue;
      }
      if (resetTimeline.exchange(false)) {
        // The fractional tick goes back to zero with the position. Without this a second render
        // would start with whatever fraction the first one happened to end on, and two bounces
        // of the same project would differ — which is the property this whole file protects.
        tickCarry = 0.0L;
        // And the pass count, for the same reason: a render begins at the loop start, so it must
        // begin at pass 0 every time or two bounces of one project would differ.
        transport.transportElapsedNanotick.store(0, std::memory_order_release);
        // Rewind to the loop start (Stop), resetting the audio playback position
        // with it so the next Play begins there rather than mid-block.
        transport.transportNanotick.store(transport.loopStartNanotick.load(std::memory_order_acquire),
                                std::memory_order_release);
        audioPlaybackBlockId.store(0, std::memory_order_release);
      }

      for (auto* runtime : trackSnapshot) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          continue;
        }
        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          const uint64_t gateTime =
              runtime->mirrorGateSampleTime.load(std::memory_order_acquire);
          uint64_t ack = 0;
          {
            std::lock_guard<std::mutex> lock(runtime->controllerMutex);
            const auto* mailbox = runtime->controller.mailbox();
            if (!mailbox) {
              continue;
            }
            ack = mailbox->replayAckSampleTime.load(std::memory_order_acquire);
          }
          std::cout << "Mirror check: track " << runtime->trackId
                    << ", gateTime=" << gateTime
                    << ", ack=" << ack << std::endl;
          // A NON-ZERO GATE IS REQUIRED. Arming zeroes the gate, so without this an arm landing
          // between the primed check and here would be retired by the PREVIOUS replay's ack.
          if (daw::mirrorReplayAnswered(gateTime, ack)) {
            // Retire only the causes this replay actually answered. A cause armed after the params
            // were written is not covered by this acknowledgement and must survive it.
            const uint32_t answered = runtime->mirrorCauses.load(std::memory_order_acquire);
            retireMirrorCause(*runtime, static_cast<daw::MirrorCause>(answered));
            std::cout << "Mirror completed for track " << runtime->trackId
                      << " (causes " << answered << ")" << std::endl;
          }
        }
      }

      uint32_t minCompleted = std::numeric_limits<uint32_t>::max();
      // Reused across iterations: the producer path must not allocate per block.
      hostProgress.clear();
      bool anyActive = false;
      for (auto* runtime : trackSnapshot) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          continue;
        }
        uint32_t completed = 0;
        {
          std::unique_lock<std::mutex> lock(runtime->controllerMutex, std::try_to_lock);
          if (!lock.owns_lock()) {
            continue;
          }
          const auto* mailbox = runtime->controller.mailbox();
          if (!mailbox) {
            continue;
          }
          completed = mailbox->completedBlockId.load(std::memory_order_acquire);
        }
        if (completed > 0) {
          runtime->active.store(true, std::memory_order_release);
        }
        // COLLECTED, NOT ACCUMULATED. The rule for which hosts count toward the back-pressure
        // minimum is daw::engine::completedMinimum in apps/engine_rt_helpers.h, where it can be
        // asked a question without booting an engine — which matters because getting it wrong
        // froze the transport for every track and took a live stack sample to find. See the
        // header for the deadlock it now excludes.
        hostProgress.push_back(
            {true, completed,
             runtime->lastDispatchedBlockId.load(std::memory_order_acquire)});
      }
      // THE RULE, applied where it can be tested: apps/engine_rt_helpers.h.
      const auto progress = daw::engine::completedMinimum(
          hostProgress, nextBlockId.load(std::memory_order_relaxed));
      // WHICH host is holding the minimum, and what every host reported. "the hosts are behind"
      // is as far as anyone can get from the numbers above, and with six hosted plugins that is
      // not far enough to act on: one stuck host and six evenly-behind hosts are different bugs.
      // Built only when the stall log is armed, so it costs nothing in a normal run.
      if (debugStall) {
        std::string perHost;
        for (size_t i = 0; i < hostProgress.size(); ++i) {
          perHost += (i ? "," : "");
          perHost += std::to_string(i);
          perHost += hostProgress[i].active ? ":" : ":inactive@";
          perHost += std::to_string(hostProgress[i].completedBlockId);
        }
        stallHosts = perHost;
      }
      anyActive = progress.anyContributing;
      minCompleted = progress.minCompleted;
      const bool throttleInactive = !anyActive;
      if (minCompleted == std::numeric_limits<uint32_t>::max()) {
        if (isPlaying) {
          logStall("minCompleted", nextBlockId.load(std::memory_order_relaxed), 0,
                   audioPlaybackBlockId.load(std::memory_order_acquire), 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      const uint32_t inFlight = nextBlockId.load() - minCompleted;
      if (inFlight >= engineConfig.numBlocks) {
        if (isPlaying) {
          logStall("inFlight", nextBlockId.load(std::memory_order_relaxed), minCompleted,
                   audioPlaybackBlockId.load(std::memory_order_acquire), inFlight);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      // Also check that we're not getting too far ahead of audio playback
      // Allow producer to be ahead by at most 10 blocks for buffering
      uint32_t currentPlayback = audioPlaybackBlockId.load(std::memory_order_acquire);
      const auto playbackNow = std::chrono::steady_clock::now();
      if (currentPlayback != lastPlaybackBlock) {
        lastPlaybackBlock = currentPlayback;
        lastPlaybackAdvance = playbackNow;
      } else if (isPlaying && currentPlayback > 0 &&
                 playbackNow - lastPlaybackAdvance > playbackStallLimit) {
        const uint32_t fallback =
            minCompleted == std::numeric_limits<uint32_t>::max()
                ? (nextBlockId.load(std::memory_order_relaxed) > 0
                       ? nextBlockId.load(std::memory_order_relaxed) - 1
                       : 0)
                : minCompleted;
        audioPlaybackBlockId.store(fallback, std::memory_order_release);
        currentPlayback = fallback;
        lastPlaybackBlock = fallback;
        lastPlaybackAdvance = playbackNow;
      }
      bool throttlePlayback = false;
      if (currentPlayback > 0) {  // Only pace once device playback has started
        const uint32_t nextId = nextBlockId.load(std::memory_order_relaxed);
        // Pace production to the AUDIO DEVICE, not to how fast the hosts can
        // render. The transport advances once per produced block (transportNanotick
        // at :8369), so if production outruns playback the whole song speeds up —
        // it ran ~4.5x too fast. The block ring is numBlocks deep, so the producer
        // must not get numBlocks ahead of the block the device is actually playing
        // or it overwrites a slot the callback still needs. HARD gate: wait until
        // the device drains one. (The old code allowed being 10 ahead — impossible
        // to honour with a 4-deep ring — only under a 100ms latch, and the audio
        // callback's catch-up kept currentPlayback glued to nextId so it never even
        // reached 10. That is why the brake never engaged.)
        if (currentPlayback <= nextId &&
            nextId - currentPlayback >= engineConfig.numBlocks) {
          if (isPlaying) {
            logStall("ahead", nextId, minCompleted, currentPlayback,
                     nextId - currentPlayback);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
      } else {
        throttlePlayback = true;
        // THE RING IS STILL FINITE BEFORE THE PUMP HAS TAKEN ANYTHING.
        //
        // The gate above only engages once currentPlayback > 0. Until then production is
        // completely unthrottled — and the ring is numBlocks deep whether or not anyone has
        // read from it yet. With the default numBlocks of 3 the producer can reach block 3
        // before the pump takes block 0, and 3 % 3 == 0, so block 3's audio lands in block 0's
        // slot. The pump then writes block 3's audio to the file as the first block.
        //
        // MEASURED, not deduced. tools/slice_select_check.sh compares the same project rendered
        // at 64, 256 and 1024 frames; under a parallel ctest the 64-frame render differed from
        // the other two in exactly frames 0..63 and nowhere else, twice, in evidence kept by
        // the check's failure trap. Those 64 frames are byte-for-byte the correct signal's
        // frames 192..255 — block 3 of 64. Not a corruption, not a phase error: a whole block,
        // displaced by exactly the ring depth.
        //
        // It is load-dependent because whether the producer gets three blocks ahead before the
        // pump's first wake-up is a scheduler question, which is why the same bounce differed
        // run to run on a busy machine and never on an idle one.
        //
        // OFFLINE ONLY. Live, the device consumes at a fixed rate and a block lost before the
        // first callback is inaudible; the consumer's catch-up corrections handle it and are
        // deliberately disabled offline, because there a skipped block is a hole in the file.
        // Offline the requirement is absolute — every produced block must reach the file — and
        // there is no deadline, so the honest response to a full ring is to stop producing.
        //
        // Deadlock is not reachable: the pump publishes its cursor after consuming, so a pump
        // that is running always leaves the producer room, and a pump that never starts has
        // nothing to be starved of.
        if (offlineRender && nextBlockId.load(std::memory_order_relaxed) >=
                                 engineConfig.numBlocks) {
          std::this_thread::sleep_for(std::chrono::microseconds(200));
          continue;
        }
      }

        daw::engine::produceBlock(producerBlockDeps, trackSnapshot, isPlaying,
                                  throttleInactive, throttlePlayback);
    }
}

}  // namespace daw::engine
