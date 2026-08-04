#include "engine_produce_block.h"

// What the block body reaches for beyond the module header. This file arrived carrying
// main.cpp's includes, which described where it used to live rather than what it uses.
#include "engine_audio_callback.h"
#include "engine_producer_helpers.h"
#include "engine_pure.h"
#include "engine_render_track.h"
#include "engine_rt_helpers.h"
#include "event_log.h"


namespace daw::engine {

// MOVED FROM main.cpp FILE SCOPE. Neither has a user left there once the block body is here, and
// a second copy of either would be a divergence waiting to happen. keyCodeToPitch was in an
// anonymous namespace, so nothing outside main.cpp could name it — the same confinement that
// kept EngineAudioCallback stuck for 1,190 lines.
namespace {
int keyCodeToPitch(int keyCode) {
  switch (keyCode) {
    // Lower octave (C4..): Z S X D C V G B H N J M
    case 'Z': return 60; case 'S': return 61; case 'X': return 62; case 'D': return 63;
    case 'C': return 64; case 'V': return 65; case 'G': return 66; case 'B': return 67;
    case 'H': return 68; case 'N': return 69; case 'J': return 70; case 'M': return 71;
    // Upper octave (C5..): Q 2 W 3 E R 5 T 6 Y 7 U I
    case 'Q': return 72; case '2': return 73; case 'W': return 74; case '3': return 75;
    case 'E': return 76; case 'R': return 77; case '5': return 78; case 'T': return 79;
    case '6': return 80; case 'Y': return 81; case '7': return 82; case 'U': return 83;
    case 'I': return 84;
    default: return -1;
  }
}

const TrackStateSnapshot kEmptyTrackState{};
}  // namespace

void produceBlock(ProducerBlockDeps& deps,
                  const std::vector<TrackRuntime*>& trackSnapshot,
                  bool isPlaying, bool throttleInactive, bool throttlePlayback) {
  auto& blockDuration = deps.blockDuration;
  auto& blockTicksFor = deps.blockTicksFor;
  auto& debugStall = deps.debugStall;
  auto& engineConfig = deps.engineConfig;
  auto enqueuePreview = [&](uint32_t t_, uint8_t p_, uint8_t v_, bool on_) {
    deps.previewQueue.enqueuePreview(t_, p_, v_, on_);
  };
  auto& getHarmonyAt = deps.getHarmonyAt;
  auto& getRingCtrl = deps.getRingCtrl;
  auto& getRingStd = deps.getRingStd;
  auto& getScaleForHarmony = deps.getScaleForHarmony;
  auto& lastOverflowTick = deps.lastOverflowTick;
  auto& latencyMgr = deps.latencyMgr;
  auto& loopEndNanotick = deps.transport.loopEndNanotick;
  auto& loopStartNanotick = deps.transport.loopStartNanotick;
  auto& meterSnapshot = deps.songTiming.meterSnapshot;
  auto& nextBlockId = deps.nextBlockId;
  auto& nextNoteId = deps.nextNoteId;
  auto& offlineRender = deps.offlineRender;
  auto& panicPending = deps.panicPending;
  auto& patcherGraphSnapshot = deps.patcherGraph.patcherGraphSnapshot;
  auto& patcherParallel = deps.patcherParallel;
  auto& patcherPool = deps.patcherPool;
  auto& pendingPreviewNotes = deps.previewQueue.pendingPreviewNotes;
  auto& playing = deps.transport.playing;
  auto& poolAlwaysOn = deps.renderPoolOwner.poolAlwaysOn;
  auto& poolEngaged = deps.renderPoolOwner.poolEngaged;
  auto& poolWorkEwmaUs = deps.renderPoolOwner.poolWorkEwmaUs;
  auto& previewMutex = deps.previewQueue.previewMutex;
  auto& producerBlockBudgetUs = deps.producerBlockBudgetUs;
  auto& producerBlockUsMax = deps.producerTelemetry.producerBlockUsMax;
  auto& producerBlockUsTotal = deps.producerTelemetry.producerBlockUsTotal;
  auto& producerBlocksOverBudget = deps.producerTelemetry.producerBlocksOverBudget;
  auto& producerBlocksTimed = deps.producerTelemetry.producerBlocksTimed;
  auto& producerSamplerUsMax = deps.producerTelemetry.producerSamplerUsMax;
  auto& producerSamplerUsTotal = deps.producerTelemetry.producerSamplerUsTotal;
  auto& projectSeed = deps.projectSeed;
  auto& publishedCallback = deps.publishedCallback;
  auto& quantizePitch = deps.quantizePitch;
  auto& renderPool = deps.renderPoolOwner.renderPool;
  auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
  auto& songTimeSigDen = deps.songTiming.songTimeSigDen;
  auto& songTimeSigNum = deps.songTiming.songTimeSigNum;
  auto& tempoProvider = deps.tempoProvider;
  auto& tickConverter = deps.tickConverter;
  auto& traceNotes = deps.traceNotes;
  auto& transportElapsedNanotick = deps.transport.transportElapsedNanotick;
  auto& transportNanotick = deps.transport.transportNanotick;
  auto& patternTicks = deps.patternTicks;
  auto& warnedEventOutsideBlock = deps.warnedEventOutsideBlock;
  auto& writeMirrorParams = deps.writeMirrorParams;

      auto findTrackRuntime = [&](uint32_t trackId) -> TrackRuntime* {
        for (auto* runtime : trackSnapshot) {
          if (runtime && runtime->trackId == trackId) {
            return runtime;
          }
        }
        return nullptr;
      };

      const uint32_t blockId = nextBlockId.fetch_add(1);
      // Everything from here to the bottom of the loop is THIS block's production. The waits
      // above are deliberately outside it: sleeping because the device has not drained a slot
      // yet is the pipeline working, not the producer struggling, and folding that idle time in
      // would report a healthy engine as loaded.
      const auto blockWorkStart = std::chrono::steady_clock::now();
      // Summed across every track, and once the pool is running that means across threads —
      // so this is sampler CPU time, which can exceed the block's wall clock. That is the
      // number worth having: it says how much sampler work the block contained, independently
      // of how many threads it was spread over.
      std::atomic<uint64_t> blockSamplerUs{0};
      const uint64_t sampleStart =
          static_cast<uint64_t>(engineConfig.blockSize) *
          static_cast<uint64_t>(blockId - 1);

      const uint64_t pluginSampleStart = latencyMgr.getCompensatedStart(sampleStart);
      const auto loop = daw::engine::effectiveLoop(
          loopStartNanotick.load(std::memory_order_acquire),
          loopEndNanotick.load(std::memory_order_acquire), patternTicks);
      const uint64_t loopStartTicks = loop.startTick;
      const uint64_t loopEndTicks = loop.endTick;
      const uint64_t loopLen =
          loopEndTicks > loopStartTicks ? loopEndTicks - loopStartTicks : 0;
      auto wrapTick = [&](uint64_t tick) -> uint64_t {
        return daw::engine::wrapTickIntoLoop(tick, loopStartTicks, loopEndTicks);
      };

      uint64_t blockStartTicks =
          transportNanotick.load(std::memory_order_acquire);
      blockStartTicks = wrapTick(blockStartTicks);
      const uint64_t blockTicks = blockTicksFor(blockStartTicks);
      const uint64_t blockEndTicks = blockStartTicks + blockTicks;
      // Stamp where this block sits on the timeline so the callback can place audio
      // regions at the same instant as this block's MIDI. Absolute, so it is correct
      // across tempo changes.
      if (auto* cb = publishedCallback()) {
        cb->setBlockStartSample(
            blockId, static_cast<uint64_t>(
                         tickConverter.nanoticksToSamplesAbsolute(blockStartTicks)));
      }

      daw::engine::RenderTrackDeps renderTrackDeps{
          engineConfig,
          deps.harmonyTimeline,
          lastOverflowTick,
          latencyMgr,
          nextNoteId,
          deps.patcherGraph,
          patcherParallel,
          patcherPool,
          projectSeed,
          tempoProvider,
          traceNotes,
          deps.transport,
          warnedEventOutsideBlock,
          getHarmonyAt,
          getScaleForHarmony,
          quantizePitch,
          resolveDevicePluginPath,
          wrapTick};
      auto renderTrack = [&](TrackRuntime& runtime,
                             const TrackStateSnapshot& trackState,
                             uint64_t windowStartTicks,
                             uint64_t windowEndTicks,
                             uint64_t blockSampleStart,
                             uint32_t currentBlockId,
                             daw::EventRingView& ringStd,
                             std::vector<daw::EventEntry>* routedMidi) -> bool {
        return daw::engine::renderTrack(
            renderTrackDeps, runtime, trackState, windowStartTicks, windowEndTicks,
            blockSampleStart, currentBlockId, ringStd, routedMidi, blockTicks,
            loopStartTicks, loopEndTicks, loopLen);
      };

      auto runAudioPatcherNode = [&](TrackRuntime& runtime,
                                     const daw::PatcherGraph& graphSnapshot,
                                     const std::vector<daw::ModLink>& modLinks,
                                     uint32_t nodeId,
                                     uint32_t deviceId,
                                     const float* const* inputChannels,
                                     float* modOutputsBuffer,
                                     float* modSamplesBuffer) -> bool {
        if (nodeId >= graphSnapshot.nodes.size()) {
          return false;
        }
        const auto& node = graphSnapshot.nodes[nodeId];
        if (node.type != daw::PatcherNodeType::AudioPassthrough) {
          return false;
        }
        const uint32_t channels = engineConfig.numChannelsOut;
        if (runtime.patcherAudioChannels.size() != channels) {
          runtime.patcherAudioChannels.resize(channels);
        }
        if (runtime.patcherAudioBuffer.size() !=
            static_cast<size_t>(channels) * engineConfig.blockSize) {
          runtime.patcherAudioBuffer.assign(
              static_cast<size_t>(channels) * engineConfig.blockSize, 0.0f);
        }
        for (uint32_t ch = 0; ch < channels; ++ch) {
          runtime.patcherAudioChannels[ch] =
              runtime.patcherAudioBuffer.data() +
              static_cast<size_t>(ch) * engineConfig.blockSize;
          if (inputChannels && inputChannels[ch]) {
            std::memcpy(runtime.patcherAudioChannels[ch], inputChannels[ch],
                        static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
          } else {
            std::fill(runtime.patcherAudioChannels[ch],
                      runtime.patcherAudioChannels[ch] + engineConfig.blockSize, 0.0f);
          }
        }
        daw::PatcherContext ctx{};
        ctx.abi_version = daw::kPatcherAbiVersion;
        ctx.node_id = nodeId;
        ctx.seed = projectSeed.load(std::memory_order_relaxed);
        ctx.block_start_tick = blockStartTicks;
        ctx.block_end_tick = blockEndTicks;
        ctx.block_start_sample = sampleStart;
        ctx.sample_rate = static_cast<float>(engineConfig.sampleRate);
        const double bpm = tempoProvider.bpmAtNanotick(blockStartTicks);
        ctx.tempo_bpm = static_cast<float>(bpm > 0.0 ? bpm : 120.0);
        ctx.num_frames = engineConfig.blockSize;
        ctx.event_buffer = nullptr;
        ctx.event_capacity = 0;
        ctx.event_count = nullptr;
        ctx.last_overflow_tick =
            reinterpret_cast<uint64_t*>(&lastOverflowTick);
        ctx.audio_channels = runtime.patcherAudioChannels.data();
        ctx.num_channels = channels;
        if (modOutputsBuffer) {
          std::fill(modOutputsBuffer,
                    modOutputsBuffer + kPatcherMaxModOutputs,
                    0.0f);
        }
        ctx.mod_outputs = modOutputsBuffer;
        ctx.mod_output_count = kPatcherMaxModOutputs;
        ctx.mod_output_samples = modSamplesBuffer;
        ctx.mod_output_stride = engineConfig.blockSize;
        ctx.mod_inputs = nullptr;
        ctx.mod_input_count = 0;
        ctx.mod_input_stride = 0;
        if (deviceId != daw::kDeviceIdAuto) {
          if (!modLinks.empty()) {
            auto& modInputs = runtime.audioModInputSamples;
            const size_t sampleCount =
                static_cast<size_t>(kPatcherMaxModOutputs) *
                static_cast<size_t>(engineConfig.blockSize);
            if (modInputs.size() != sampleCount) {
              modInputs.assign(sampleCount, 0.0f);
            } else {
              std::fill(modInputs.begin(), modInputs.end(), 0.0f);
            }
            const size_t stride = static_cast<size_t>(engineConfig.blockSize);
            for (const auto& link : modLinks) {
              if (!link.enabled || link.rate != daw::ModRate::SampleRate) {
                continue;
              }
              if (link.target.deviceId != deviceId) {
                continue;
              }
              if (link.target.kind != daw::ModTargetKind::PatcherParam &&
                  link.target.kind != daw::ModTargetKind::PatcherMacro) {
                continue;
              }
              if (link.source.kind != daw::ModSourceKind::PatcherNodeOutput) {
                continue;
              }
              if (link.target.targetId >= kPatcherMaxModOutputs ||
                  link.source.sourceId >= kPatcherMaxModOutputs) {
                continue;
              }
              uint32_t sourceIndex = daw::kDeviceIdAuto;
              for (uint32_t i = 0; i < runtime.modOutputDeviceIds.size(); ++i) {
                if (runtime.modOutputDeviceIds[i] == link.source.deviceId) {
                  sourceIndex = i;
                  break;
                }
              }
              if (sourceIndex == daw::kDeviceIdAuto ||
                  runtime.modOutputSamples.empty()) {
                continue;
              }
              const size_t sourceBase =
                  (static_cast<size_t>(sourceIndex) *
                       static_cast<size_t>(kPatcherMaxModOutputs) +
                   link.source.sourceId) *
                  stride;
              const float* source = runtime.modOutputSamples.data() + sourceBase;
              float* target =
                  modInputs.data() + static_cast<size_t>(link.target.targetId) * stride;
              for (size_t i = 0; i < stride; ++i) {
                target[i] += link.bias + link.depth * source[i];
              }
            }
            ctx.mod_inputs = modInputs.data();
            ctx.mod_input_count = kPatcherMaxModOutputs;
            ctx.mod_input_stride = engineConfig.blockSize;
          }
        }
        ctx.node_config = nullptr;
        ctx.node_config_size = 0;
        ctx.harmony_snapshot = nullptr;
        ctx.harmony_count = 0;
        dispatchRustKernel(node.type, ctx);
        if (deviceId != daw::kDeviceIdAuto) {
          std::lock_guard<std::mutex> lock(runtime.modSourcesMutex);
          auto& sources = runtime.modSources;
          for (uint32_t i = 0; i < ctx.mod_output_count; ++i) {
            bool updated = false;
            for (auto& source : sources) {
              if (source.ref.deviceId == deviceId &&
                  source.ref.sourceId == i &&
                  source.ref.kind == daw::ModSourceKind::PatcherNodeOutput) {
                source.value = modOutputsBuffer ? modOutputsBuffer[i] : 0.0f;
                updated = true;
                break;
              }
            }
            if (!updated) {
              daw::ModSourceState state{};
              state.ref.deviceId = deviceId;
              state.ref.sourceId = i;
              state.ref.kind = daw::ModSourceKind::PatcherNodeOutput;
              state.value = modOutputsBuffer ? modOutputsBuffer[i] : 0.0f;
              sources.push_back(state);
            }
          }
        }
        return true;
      };

      bool mirrorOnly = false;
      for (auto* runtime : trackSnapshot) {
        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            !runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          mirrorOnly = true;
          std::cout << "Producer: mirrorOnly=true (track " << runtime->trackId
                    << " pending mirror)" << std::endl;
          break;
        }
      }

      // Drain queued keyjazz auditions once for this block; the per-track loop below
      // injects each into its track's event ring. Reqs for a track that is absent or
      // whose host isn't ready this block are simply not written (silence, no error).
      std::vector<PreviewNoteReq> previewThisBlock;
      {
        std::lock_guard<std::mutex> lock(previewMutex);
        previewThisBlock.swap(pendingPreviewNotes);
      }
      // PANIC: claimed once for this block, then applied to every track below. Consuming it
      // here (rather than per track) guarantees one pass emits it to ALL tracks — a flag
      // cleared inside the loop would only reach whichever track happened to be first.
      const bool doPanic = panicPending.exchange(false, std::memory_order_acq_rel);

      // ONE TRACK'S WHOLE BLOCK. Lifted out of the `for` it used to be so it can run on the
      // render pool; the body below is otherwise unchanged, and the four guard clauses that
      // were `continue` are now `return` because "skip this track" is what they always meant.
      auto processTrack = [&](TrackRuntime* runtime) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          return;
        }
        auto trackStatePtr = std::atomic_load_explicit(&runtime->trackSnapshot,
                                                       std::memory_order_acquire);
        const auto& trackState = trackStatePtr ? *trackStatePtr : kEmptyTrackState;
        // Did another track route audio into this one this block? Read where the inbound
        // buffer is swapped in, used where the sampler decides whether to overwrite it.
        bool routedAudioArrived = false;
        // TRY-LOCK IN REALTIME, BLOCKING WAIT OFFLINE.
        //
        // Skipping a track's whole block when this mutex is contended is the right realtime
        // trade: a late block is worse than a dropped one when a device is waiting. It is the
        // WRONG trade offline, and it was making renders non-reproducible — measured at roughly
        // one run in six, differing by exactly one block somewhere in the middle, which is the
        // hardest possible way for it to fail.
        //
        // The offline render already inverts the other two policies for the same reason (never
        // skip a block, never prime with silence, never starve — WAIT). This is the third, and
        // it was simply missed: nothing routed audio THROUGH a host in an offline render until
        // the sampler did, so a skipped block used to cost a note to a plugin and now costs a
        // hole in a sustaining voice. Found by tools/sampler_determinism_check.sh.
        std::unique_lock<std::mutex> lock =
            offlineRender ? std::unique_lock<std::mutex>(runtime->controllerMutex)
                          : std::unique_lock<std::mutex>(runtime->controllerMutex,
                                                         std::try_to_lock);
        if (!lock.owns_lock()) {
          return;
        }
        if (!runtime->controller.shmHeader()) {
          return;
        }
        auto ringCtrl = getRingCtrl(*runtime);
        auto ringStd = getRingStd(*runtime);
        if (ringCtrl.mask == 0 || ringStd.mask == 0) {
          return;
        }

        // Keystroke forwarding (kControlVersion 10): drain any keys this track's plugin
        // editor forwarded and turn them into transport / keyjazz. Only a focused editor
        // ever writes here, so an idle track's ring is simply empty. Space toggles play;
        // the tracker key rows audition a pitch via the same out-of-band PreviewNote path
        // (held: keydown = note-on, keyup = note-off).
        if (auto keyShm = runtime->controller.sharedMemory();
            keyShm && keyShm->base && keyShm->header) {
          auto keyRing =
              daw::makeEventRing(keyShm->base, daw::hostKeyRingOffset(*keyShm->header));
          if (keyRing.mask != 0) {
            daw::EventEntry keyEntry;
            while (daw::ringPop(keyRing, keyEntry)) {
              if (keyEntry.type != static_cast<uint16_t>(daw::EventType::HostKey)) {
                continue;
              }
              daw::KeyEventPayload kp{};
              std::memcpy(&kp, keyEntry.payload, sizeof(kp));
              const bool down = kp.isDown != 0;
              if (kp.keyCode == 32) {  // space -> transport toggle, on keydown
                if (down) {
                  playing.store(!playing.load(std::memory_order_acquire),
                                std::memory_order_release);
                }
              } else {
                const int pitch = keyCodeToPitch(kp.keyCode);
                if (pitch >= 0) {
                  enqueuePreview(runtime->trackId, static_cast<uint8_t>(pitch),
                                 down ? 100 : 0, down);
                }
              }
            }
          }
        }

        daw::EventEntry transportEntry;
        transportEntry.sampleTime = pluginSampleStart;
        transportEntry.blockId = blockId;
        transportEntry.type = static_cast<uint16_t>(daw::EventType::Transport);
        transportEntry.size = sizeof(daw::TransportPayload);
        daw::TransportPayload transportPayload;
        // Current-position tempo (not the initial one) so a tempo-synced plugin
        // follows tempo_map changes, matching the ProcessBlockRequest play head.
        transportPayload.tempoBpm = tempoProvider.bpmAtNanotick(blockStartTicks);
        transportPayload.timeSigNum = songTimeSigNum.load(std::memory_order_relaxed);
        transportPayload.timeSigDen = songTimeSigDen.load(std::memory_order_relaxed);
        transportPayload.playState = isPlaying ? 1 : 0;
        std::memcpy(transportEntry.payload, &transportPayload, sizeof(transportPayload));
        daw::ringWrite(ringCtrl, transportEntry);

        // Inject this track's queued keyjazz auditions at the block boundary. Out of band:
        // these come straight from the keyboard, never the clip store, so they play (and
        // hold, and sustain in chords) without being recorded. Note-off carries no noteId;
        // the plugin matches it by pitch+channel. Plays whether or not the transport runs.
        if (doPanic) {
          // All-sound-off on EVERY channel, ahead of anything else this block. CC120 is
          // what makes this a panic: CC123 (all-notes-off) merely releases held notes and
          // lets a pad or reverb tail ring out. Both are sent — 123 for plugins that
          // ignore 120 — with 120 last so it wins. Every channel, because a multitimbral
          // plugin or a MIDI-per-bus instrument can be sounding on any of them.
          for (uint8_t ch = 0; ch < 16; ++ch) {
            for (const uint8_t cc : {uint8_t{123}, uint8_t{120}}) {
              daw::EventEntry panicEntry;
              panicEntry.sampleTime = pluginSampleStart;
              panicEntry.blockId = blockId;
              panicEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
              panicEntry.size = sizeof(daw::MidiPayload);
              daw::MidiPayload panicPayload{};
              panicPayload.status = 0xB0;  // control change
              panicPayload.data1 = cc;
              panicPayload.data2 = 0;
              panicPayload.channel = ch;
              std::memcpy(panicEntry.payload, &panicPayload, sizeof(panicPayload));
              daw::ringWrite(ringStd, panicEntry);
            }
          }
          // Drop this track's own note bookkeeping too. Without this the engine would
          // later emit note-offs for voices the panic already cut, and a scheduled
          // retrigger would fire after the panic — the sound coming back on its own is
          // exactly what makes a panic button untrustworthy.
          {
            std::lock_guard<std::mutex> lock(runtime->activeNotesMutex);
            runtime->activeNotes.clear();
            runtime->activeNoteByColumn.clear();
            runtime->pendingStrikes.clear();
          }
        }
        for (const auto& req : previewThisBlock) {
          if (req.trackId != runtime->trackId) {
            continue;
          }
          daw::EventEntry previewEntry;
          previewEntry.sampleTime = pluginSampleStart;
          previewEntry.blockId = blockId;
          previewEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
          previewEntry.size = sizeof(daw::MidiPayload);
          daw::MidiPayload previewPayload{};
          previewPayload.status = req.on ? 0x90 : 0x80;
          previewPayload.data1 = req.pitch;
          previewPayload.data2 = req.on ? req.velocity : 0;
          previewPayload.channel = 0;
          previewPayload.tuningCents = 0;
          previewPayload.noteId =
              req.on ? nextNoteId.fetch_add(1, std::memory_order_acq_rel) : 0;
          std::memcpy(previewEntry.payload, &previewPayload, sizeof(previewPayload));
          daw::ringWrite(ringStd, previewEntry);
        }

        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            !runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          std::cout << "Priming mirror for track " << runtime->trackId
                    << " at sample " << pluginSampleStart << std::endl;
          writeMirrorParams(*runtime, trackState, pluginSampleStart);
          runtime->mirrorPrimed.store(true, std::memory_order_release);
          std::cout << "Mirror primed for track " << runtime->trackId
                    << ", gate sample time = "
                    << runtime->mirrorGateSampleTime.load() << std::endl;
        }

        const auto& routingSnapshot = trackState.routing;

        auto enqueueInboundAudio = [&](TrackRuntime& dst,
                                       const float* const* channels) {
          if (!channels) {
            return;
          }
          const size_t expectedSamples =
              static_cast<size_t>(engineConfig.blockSize) *
              static_cast<size_t>(engineConfig.numChannelsOut);
          std::lock_guard<std::mutex> lock(dst.inboundMutex);
          dst.inboundAudioArrived.store(true, std::memory_order_relaxed);
          if (dst.inboundAudioBuffer.size() != expectedSamples) {
            dst.inboundAudioBuffer.assign(expectedSamples, 0.0f);
          }
          for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
            const float* input = channels[ch];
            if (!input) {
              continue;
            }
            float* dest = dst.inboundAudioBuffer.data() +
                static_cast<size_t>(ch) * engineConfig.blockSize;
            for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
              dest[i] += input[i];
            }
          }
        };

        auto enqueueInboundMidi = [&](TrackRuntime& dst,
                                      const std::vector<daw::EventEntry>& events,
                                      uint64_t blockSampleStart,
                                      uint64_t nextBlockSampleStart) {
          if (events.empty()) {
            return;
          }
          std::lock_guard<std::mutex> lock(dst.inboundMutex);
          for (const auto& entry : events) {
            if (entry.type != static_cast<uint16_t>(daw::EventType::Midi)) {
              continue;
            }
            if (entry.sampleTime < blockSampleStart) {
              continue;
            }
            const uint64_t offset = entry.sampleTime - blockSampleStart;
            daw::EventEntry routed = entry;
            routed.sampleTime = nextBlockSampleStart + offset;
            routed.blockId = 0;
            dst.inboundMidiEvents.push_back(routed);
          }
        };

        {
          std::lock_guard<std::mutex> lock(runtime->inboundMutex);
          const size_t expectedSamples =
              static_cast<size_t>(engineConfig.blockSize) *
              static_cast<size_t>(engineConfig.numChannelsOut);
          if (runtime->inputAudioBuffer.size() != expectedSamples) {
            runtime->inputAudioBuffer.assign(expectedSamples, 0.0f);
            runtime->inputAudioChannels.resize(engineConfig.numChannelsOut);
            for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
              runtime->inputAudioChannels[ch] =
                  runtime->inputAudioBuffer.data() +
                  static_cast<size_t>(ch) * engineConfig.blockSize;
            }
          }
          routedAudioArrived = runtime->inboundAudioArrived.exchange(
              false, std::memory_order_relaxed);
          if (runtime->inboundAudioBuffer.size() == expectedSamples) {
            std::copy(runtime->inboundAudioBuffer.begin(),
                      runtime->inboundAudioBuffer.end(),
                      runtime->inputAudioBuffer.begin());
            std::fill(runtime->inboundAudioBuffer.begin(),
                      runtime->inboundAudioBuffer.end(),
                      0.0f);
          } else {
            std::fill(runtime->inputAudioBuffer.begin(),
                      runtime->inputAudioBuffer.end(),
                      0.0f);
          }
        }

        bool patcherAudioWritten = false;
        std::vector<daw::EventEntry> routedMidi;
        if (!mirrorOnly && isPlaying) {
          patcherAudioWritten = renderTrack(*runtime, trackState,
                                            blockStartTicks, blockEndTicks,
                                            sampleStart, blockId, ringStd,
                                            routingSnapshot.midiOut.kind ==
                                                    daw::TrackRouteKind::Track
                                                ? &routedMidi
                                                : nullptr);
        } else if (mirrorOnly) {
          std::cout << "Producer: Skipping renderTrack for track "
                    << runtime->trackId << " (mirrorOnly)" << std::endl;
        }

        // Movement 4 MIDI-per-bus: render each aux child's notes into THIS parent host's
        // ring — tagged (inside renderTrack) with the child's bus MIDI channel — before
        // the parent's ProcessBlock, so a multitimbral instrument routes channel k to its
        // output bus k and the child's audio is that bus's stem. Same single producer
        // thread + same ring, so there is no writer race.
        if (!mirrorOnly && isPlaying) {
          for (auto* child : trackSnapshot) {
            if (!child->isAuxChild.load(std::memory_order_acquire) ||
                child->auxParentTrackId.load(std::memory_order_relaxed) !=
                    runtime->trackId) {
              continue;
            }
            // MIDI has 16 channels (0..15); channel 0 is the parent's own bus. A child
            // for aux bus >= 16 has no distinct channel to steer on (16 & 0x0F == 0 would
            // alias onto the parent's channel), so skip its MIDI. Its AUDIO still works
            // — the aux plane carries 32 channels = up to 16 stereo stems.
            if (child->auxBusIndex.load(std::memory_order_relaxed) > 15u) {
              continue;
            }
            auto childStatePtr = std::atomic_load_explicit(
                &child->trackSnapshot, std::memory_order_acquire);
            const auto& childState =
                childStatePtr ? *childStatePtr : kEmptyTrackState;
            renderTrack(*child, childState, blockStartTicks, blockEndTicks,
                        sampleStart, blockId, ringStd, nullptr);
          }
        }

        struct SegmentInfo {
          uint16_t start = 0;
          uint16_t length = 0;
          struct AudioNodeInfo {
            uint32_t nodeId = 0;
            uint32_t deviceId = 0;
          };
          std::vector<AudioNodeInfo> audioNodeIds;
        };
        std::vector<SegmentInfo> segments;
        segments.reserve(trackState.chainDevices.size());
        std::vector<SegmentInfo::AudioNodeInfo> pendingAudioNodes;
        pendingAudioNodes.reserve(trackState.chainDevices.size());
        uint16_t hostIndex = 0;
        bool inSegment = false;
        uint16_t segmentStart = 0;
        uint16_t segmentLength = 0;
        for (const auto& device : trackState.chainDevices) {
          const bool isVst = device.kind == daw::DeviceKind::VstInstrument ||
              device.kind == daw::DeviceKind::VstEffect;
          if (isVst) {
            if (!resolveDevicePluginPath(*runtime, device.hostSlotIndex)) {
              continue;
            }
            if (!inSegment) {
              if (!segments.empty() && !pendingAudioNodes.empty()) {
                segments.back().audioNodeIds.insert(
                    segments.back().audioNodeIds.end(),
                    pendingAudioNodes.begin(),
                    pendingAudioNodes.end());
                pendingAudioNodes.clear();
              }
              inSegment = true;
              segmentStart = hostIndex;
              segmentLength = 0;
            }
            segmentLength++;
            hostIndex++;
          } else {
            if (inSegment) {
              SegmentInfo info;
              info.start = segmentStart;
              info.length = segmentLength;
              segments.push_back(info);
              inSegment = false;
              segmentLength = 0;
            }
            if (!device.bypass && device.kind == daw::DeviceKind::PatcherAudio) {
              SegmentInfo::AudioNodeInfo info{};
              info.nodeId = device.patcherNodeId;
              info.deviceId = device.id;
              pendingAudioNodes.push_back(info);
            }
          }
        }
        if (inSegment) {
          SegmentInfo info;
          info.start = segmentStart;
          info.length = segmentLength;
          segments.push_back(info);
        }
        if (!segments.empty() && !pendingAudioNodes.empty()) {
          segments.back().audioNodeIds.insert(
              segments.back().audioNodeIds.end(),
              pendingAudioNodes.begin(),
              pendingAudioNodes.end());
          pendingAudioNodes.clear();
        }
        if (segments.empty()) {
          SegmentInfo info;
          info.start = 0;
          info.length = 0;
          segments.push_back(info);
        }

        auto audioGraphPtr = std::atomic_load_explicit(&patcherGraphSnapshot,
                                                       std::memory_order_acquire);
        static const daw::PatcherGraph kEmptyAudioGraph{};
        const daw::PatcherGraph& audioGraphSnapshot =
            audioGraphPtr ? *audioGraphPtr : kEmptyAudioGraph;

        const uint32_t blockIndex = blockId % engineConfig.numBlocks;
          // ---- THE BUILT-IN SAMPLER RENDERS HERE, on the PRODUCER thread.
        //
        // Not in the audio callback (which only consumes finished blocks, so the sampler is off
        // the hardest-deadline thread by construction) and not into the master sum (which has
        // already passed every plugin, so a VST effect could never follow the sampler on the
        // same track). Its output goes into the host input plane below, AHEAD of the chain.
        runtime->samplerAudioValid = false;
        // ONE STRONG REFERENCE for this whole block. snapshot() used to hand back a bare
        // pointer, and the command thread could free the snapshot between this null check and
        // the stemCount read below — which is exactly the use-after-free ThreadSanitizer named.
        const std::shared_ptr<const daw::SamplerRender> samplerSnap =
            runtime->samplerRuntime.snapshot();
        if (runtime->samplerDeviceId.load(std::memory_order_acquire) != 0 && samplerSnap) {
          const uint32_t channels = std::max<uint32_t>(engineConfig.numChannelsOut, 2u);
          const size_t need = static_cast<size_t>(channels) * engineConfig.blockSize;
          if (runtime->samplerAudioBuffer.size() != need) {
            runtime->samplerAudioBuffer.assign(need, 0.0f);
          } else {
            std::fill(runtime->samplerAudioBuffer.begin(), runtime->samplerAudioBuffer.end(),
                      0.0f);
          }
          if (runtime->samplerAudioChannels.size() != channels) {
            runtime->samplerAudioChannels.resize(channels);
          }
          for (uint32_t ch = 0; ch < channels; ++ch) {
            runtime->samplerAudioChannels[ch] = runtime->samplerAudioBuffer.data() +
                                                static_cast<size_t>(ch) * engineConfig.blockSize;
          }
          // The event list is built in emit order, which is tick order, but a retrigger's
          // strikes and a note-off scheduled earlier in the same block can interleave — so it is
          // sorted rather than assumed. stable_sort because two events at one sample must keep
          // the order they were emitted in: a note-off and the note-on that replaces it landing
          // on the same frame is a repeat, and swapping them would cut the NEW note.
          std::stable_sort(runtime->samplerEvents.begin(), runtime->samplerEvents.end(),
                           [](const daw::SamplerEvent& a, const daw::SamplerEvent& b) {
                             return a.offsetInBlock < b.offsetInBlock;
                           });
          // Nanoticks per frame for THIS block, for tempo-synced envelopes. Recomputed per block
          // rather than cached: under a tempo ramp a stale ratio detunes every running envelope.
          const double bpmNow = tempoProvider.bpmAtNanotick(blockStartTicks);
          runtime->samplerRuntime.setNanotickPerFrame(
              (bpmNow > 0.0 ? bpmNow : 120.0) *
              static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter) /
              (60.0 * engineConfig.sampleRate));
          // STEMS. A slot with outputStem != 0 renders into its own stereo pair in the AUX
          // INPUT region — the last numAuxChannelsOut channels of the input plane — which the
          // host copies to the aux OUTPUT plane, where reconcileChildTracks reads it. The
          // sampler's stems therefore travel the same route as a multi-out plugin's, and the
          // child-track machinery does not need to know which produced them.
          const uint32_t stems = samplerSnap->state.stemCount;
          std::vector<float*> stemPlanes;
          if (stems > 0) {
            const size_t need = static_cast<size_t>(stems) * 2 * engineConfig.blockSize;
            if (runtime->samplerStemBuffer.size() != need) {
              runtime->samplerStemBuffer.assign(need, 0.0f);
            } else {
              std::fill(runtime->samplerStemBuffer.begin(), runtime->samplerStemBuffer.end(),
                        0.0f);
            }
            stemPlanes.resize(static_cast<size_t>(stems) * 2);
            for (size_t i = 0; i < stemPlanes.size(); ++i) {
              stemPlanes[i] =
                  runtime->samplerStemBuffer.data() + i * engineConfig.blockSize;
            }
          }
          // Timed separately from the block as a whole: this is the part that scales with the
          // number of sampler tracks and the voices in them, so it is the part that answers
          // "is the sampler what saturated the producer" without guessing.
          const auto samplerStart = std::chrono::steady_clock::now();
          runtime->samplerRuntime.render(
              runtime->samplerAudioChannels.data(), channels, engineConfig.blockSize,
              runtime->samplerEvents.empty() ? nullptr : runtime->samplerEvents.data(),
              static_cast<uint32_t>(runtime->samplerEvents.size()),
              stemPlanes.empty() ? nullptr : stemPlanes.data(), stems);
          blockSamplerUs.fetch_add(
              static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - samplerStart)
                      .count()),
              std::memory_order_relaxed);
          runtime->samplerStemCount = stems;
          runtime->samplerAudioValid = true;
        }
        runtime->samplerEvents.clear();



        bool patcherAudioValid = patcherAudioWritten;
        if (patcherAudioValid && !runtime->inputAudioChannels.empty()) {
          const uint32_t channels =
              static_cast<uint32_t>(runtime->inputAudioChannels.size());
          for (uint32_t ch = 0; ch < channels; ++ch) {
            const float* input = runtime->inputAudioChannels[ch];
            float* output =
                ch < runtime->patcherAudioChannels.size()
                    ? runtime->patcherAudioChannels[ch]
                    : nullptr;
            if (!input || !output) {
              continue;
            }
            for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
              output[i] += input[i];
            }
          }
        }
        auto& outputPtrs = runtime->audioOutputPtrs;
        if (outputPtrs.size() != engineConfig.numChannelsOut) {
          outputPtrs.resize(engineConfig.numChannelsOut, nullptr);
        } else {
          std::fill(outputPtrs.begin(), outputPtrs.end(), nullptr);
        }
        std::array<float, kPatcherMaxModOutputs> audioModOutputs{};
        auto& audioModSamples = runtime->audioModSamples;
        const size_t audioModSampleCount =
            static_cast<size_t>(kPatcherMaxModOutputs) *
            static_cast<size_t>(engineConfig.blockSize);
        if (audioModSamples.size() != audioModSampleCount) {
          audioModSamples.assign(audioModSampleCount, 0.0f);
        } else {
          std::fill(audioModSamples.begin(), audioModSamples.end(), 0.0f);
        }
        const auto* header = runtime->controller.shmHeader();
        const size_t shmSize = runtime->controller.shmSize();
        auto safeAudioInPtr = [&](uint32_t blockIndex, uint32_t channel) -> float* {
          if (!header) {
            return nullptr;
          }
          const auto offset = daw::engine::audioChannelOffset(
              header->audioInOffset, header->channelStrideBytes, header->numChannelsIn, blockIndex,
              header->numBlocks, channel, shmSize);
          if (!offset) {
            return nullptr;
          }
          return reinterpret_cast<float*>(
              reinterpret_cast<uint8_t*>(const_cast<daw::ShmHeader*>(header)) + *offset);
        };
        auto safeAudioOutPtr = [&](uint32_t blockIndex, uint32_t channel) -> float* {
          if (!header) {
            return nullptr;
          }
          const auto offset = daw::engine::audioChannelOffset(
              header->audioOutOffset, header->channelStrideBytes, header->numChannelsOut, blockIndex,
              header->numBlocks, channel, shmSize);
          if (!offset) {
            return nullptr;
          }
          return reinterpret_cast<float*>(
              reinterpret_cast<uint8_t*>(const_cast<daw::ShmHeader*>(header)) + *offset);
        };

        // Movement 4 sidechain: pull the key signal from the source track's output into
        // this track's sidechain buffer, written below into the host input plane's
        // sidechain channels [numChannelsOut, numChannelsIn). The source's latest
        // COMPLETED block is read — one to two blocks old, which a dynamics processor's
        // attack absorbs — and holding the shmView shared_ptr keeps it alive across the
        // read even if the source host restarts. Silence when unbound or not ready.
        {
          const size_t scSamples =
              static_cast<size_t>(kSidechainChannels) * engineConfig.blockSize;
          if (runtime->sidechainInputBuffer.size() != scSamples) {
            runtime->sidechainInputBuffer.assign(scSamples, 0.0f);
          } else {
            std::fill(runtime->sidechainInputBuffer.begin(),
                      runtime->sidechainInputBuffer.end(), 0.0f);
          }
          if (routingSnapshot.sidechain.kind == daw::TrackRouteKind::Track) {
            TrackRuntime* src = findTrackRuntime(routingSnapshot.sidechain.trackId);
            // Hold the SOURCE track's controllerMutex while reading its SHM: the restart
            // worker reassigns src's shmView_ (a non-atomic shared_ptr) + munmaps the old
            // SHM under this same lock, so an unsynchronized sharedMemory() copy would be
            // a data race + use-after-free. try_lock (never block) so a source restart
            // just skips the key this block; this track already holds its own
            // controllerMutex, so try-then-skip also avoids a lock-order deadlock.
            std::unique_lock<std::mutex> srcLock;
            if (src && src != runtime) {
              srcLock = std::unique_lock<std::mutex>(src->controllerMutex,
                                                     std::try_to_lock);
            }
            if (src && src != runtime && srcLock.owns_lock() &&
                src->hostReady.load(std::memory_order_acquire)) {
              auto srcView = src->controller.sharedMemory();
              if (srcView && srcView->base && srcView->header &&
                  srcView->completedBlockId) {
                const daw::ShmHeader* sh = srcView->header;
                const uint32_t completed =
                    srcView->completedBlockId->load(std::memory_order_acquire);
                const uint64_t frameBytes =
                    static_cast<uint64_t>(engineConfig.blockSize) * sizeof(float);
                if (completed > 0 && sh->numBlocks > 0 && sh->numChannelsOut > 0 &&
                    sh->channelStrideBytes >= frameBytes) {
                  const uint64_t stride = sh->channelStrideBytes;
                  const uint64_t blockBytes =
                      static_cast<uint64_t>(sh->numChannelsOut) * stride;
                  const uint64_t srcBlock =
                      static_cast<uint64_t>(completed % sh->numBlocks);
                  for (uint32_t j = 0; j < kSidechainChannels; ++j) {
                    const uint32_t srcCh =
                        j < sh->numChannelsOut ? j : (sh->numChannelsOut - 1);
                    const uint64_t off = sh->audioOutOffset + srcBlock * blockBytes +
                                         static_cast<uint64_t>(srcCh) * stride;
                    if (off + frameBytes > srcView->size) {
                      continue;
                    }
                    const float* srcChannel = reinterpret_cast<const float*>(
                        reinterpret_cast<const uint8_t*>(srcView->base) + off);
                    std::copy(srcChannel, srcChannel + engineConfig.blockSize,
                              runtime->sidechainInputBuffer.data() +
                                  static_cast<size_t>(j) * engineConfig.blockSize);
                  }
                }
              }
            }
          }
        }

        for (size_t segIndex = 0; segIndex < segments.size(); ++segIndex) {
          const auto& segment = segments[segIndex];
          const uint16_t segmentStart = segment.start;
          const uint16_t segmentLength = segment.length;
          for (uint32_t ch = 0; ch < engineConfig.numChannelsIn; ++ch) {
            float* input = safeAudioInPtr(blockIndex, ch);
            if (!input) {
              continue;
            }
            // Movement 4: channels after the main bus carry the sidechain (key) input,
            // the same for every segment (it feeds the first plugin's sidechain bus).
            if (ch >= engineConfig.numChannelsOut) {
              // THE AUX INPUT REGION is the LAST numAuxChannelsOut channels of the plane, and
              // the sidechain sits between it and the main channels. Derived here the same way
              // the host derives it, so the two cannot disagree about where the boundary is.
              const uint32_t auxInBase =
                  engineConfig.numChannelsIn > engineConfig.numAuxChannelsOut
                      ? engineConfig.numChannelsIn - engineConfig.numAuxChannelsOut
                      : engineConfig.numChannelsIn;
              if (ch >= auxInBase) {
                // A SAMPLER STEM (kControlVersion 14). The host copies these to the aux OUTPUT
                // plane, where reconcileChildTracks reads them — so the sampler's stems reach a
                // child track by the same route a multi-out plugin's do.
                const uint32_t stemCh = ch - auxInBase;
                const size_t base = static_cast<size_t>(stemCh) * engineConfig.blockSize;
                if (runtime->samplerAudioValid &&
                    base + engineConfig.blockSize <= runtime->samplerStemBuffer.size()) {
                  std::memcpy(input, runtime->samplerStemBuffer.data() + base,
                              static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
                } else {
                  std::fill(input, input + engineConfig.blockSize, 0.0f);
                }
                continue;
              }
              const size_t base = static_cast<size_t>(ch - engineConfig.numChannelsOut) *
                                  engineConfig.blockSize;
              if (base + engineConfig.blockSize <=
                  runtime->sidechainInputBuffer.size()) {
                std::memcpy(input, runtime->sidechainInputBuffer.data() + base,
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else {
                std::fill(input, input + engineConfig.blockSize, 0.0f);
              }
              continue;
            }
            if (segIndex == 0) {
              // THE SAMPLER FEEDS THE HEAD OF THE CHAIN. Only on the FIRST segment: later
              // segments carry the previous segment's OUTPUT back in, and re-injecting the
              // sampler there would make it play once per plugin run.
              //
              // It is checked before the patcher's audio because a track carrying both has the
              // sampler as its instrument and the patcher node as an effect; ordered the other
              // way, adding a patcher audio node would silently mute the sampler.
              if (runtime->samplerAudioValid && ch < runtime->samplerAudioChannels.size() &&
                  runtime->samplerAudioChannels[ch]) {
                // THE SAMPLER REPLACES THE INPUT, so a track that is both an instrument and a
                // bus destination silently loses everything routed into it. Whether that should
                // MIX instead is a real decision about what a track is (Live and Renoise mix),
                // and it is not one to make silently — so until it is made, say so out loud
                // rather than letting the audio disappear with nothing to look at. Task #92.
                if (routedAudioArrived &&
                    !runtime->warnedSamplerAteInput.exchange(true,
                                                             std::memory_order_relaxed)) {
                  DAW_EVENT("sampler.discarded_routed_input")
                      .field("track", runtime->trackId)
                      .field("note",
                             "a sampler feeds the head of the chain and REPLACES the track's "
                             "input, so audio routed into this track is not heard");
                }
                std::memcpy(input, runtime->samplerAudioChannels[ch],
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else if (patcherAudioValid && ch < runtime->patcherAudioChannels.size() &&
                  runtime->patcherAudioChannels[ch]) {
                std::memcpy(input, runtime->patcherAudioChannels[ch],
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else if (ch < runtime->inputAudioChannels.size() &&
                         runtime->inputAudioChannels[ch]) {
                std::memcpy(input, runtime->inputAudioChannels[ch],
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else {
                std::fill(input, input + engineConfig.blockSize, 0.0f);
              }
              continue;
            }
            if (patcherAudioValid && ch < runtime->patcherAudioChannels.size() &&
                runtime->patcherAudioChannels[ch]) {
              std::memcpy(input, runtime->patcherAudioChannels[ch],
                          static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              continue;
            }
            float* output = safeAudioOutPtr(blockIndex, ch);
            if (output) {
              std::memcpy(input, output,
                          static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
            } else {
              std::fill(input, input + engineConfig.blockSize, 0.0f);
            }
          }

          // Musical position for this block. Without it the hosted plugin has
          // no play head and every tempo-synced effect free-runs.
          daw::HostTransport transport;
          transport.bpm = tempoProvider.bpmAtNanotick(blockStartTicks);
          transport.ppqPosition =
              static_cast<double>(blockStartTicks) /
              static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter);
          // Quarter notes per bar = numerator * 4 / denominator (ppq counts quarters), so a 7/8
          // bar is 3.5 quarters and a tempo-synced plugin's bar start is right in any meter.
          //
          // v29: THE METER AT THE PLAYHEAD, not the song default. This read the song-wide pair,
          // so a mid-song meter change did not reach the plugins at all — every tempo-synced
          // delay and arp kept counting the opening signature through a 7/8 bridge. It was not
          // observable before because nothing could author a change; the meter map is
          // authoritative now, so it can be, and this is the read that makes it mean something.
          //
          // From an immutable snapshot, swapped atomically: the RT cannot take arrangeMutex.
          const auto meter =
              std::atomic_load_explicit(&meterSnapshot, std::memory_order_acquire);
          const daw::TimeSignature sig =
              meter ? meter->signatureAt(blockStartTicks) : daw::TimeSignature{};
          // Guarded, and this is not belt-and-braces: numerator 0 gives beatsPerBar 0 and a NaN
          // bar start, which a plugin then divides by. A load used to guard the adopted value
          // and then RE-STORE it unguarded fifty lines later, so the guard was dead.
          const double beatsPerBar =
              (sig.numerator > 0 && sig.denominator > 0)
                  ? static_cast<double>(sig.numerator) * 4.0 /
                        static_cast<double>(sig.denominator)
                  : 4.0;
          transport.ppqPositionOfLastBarStart =
              std::floor(transport.ppqPosition / beatsPerBar) * beatsPerBar;
          transport.isPlaying = playing.load(std::memory_order_acquire);

          bool sentOk = false;
          if (debugStall) {
            const auto sendStart = std::chrono::steady_clock::now();
            sentOk = runtime->controller.sendProcessBlock(blockId,
                                                          sampleStart,
                                                          pluginSampleStart,
                                                          segmentStart,
                                                          segmentLength,
                                                          transport);
            const auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - sendStart).count();
            if (sendMs > 10) {
              daw::LogLine() << "Engine: sendProcessBlock slow (track "
                        << runtime->trackId << ", " << sendMs
                        << " ms)" << std::endl;
            }
          } else {
            sentOk = runtime->controller.sendProcessBlock(blockId,
                                                          sampleStart,
                                                          pluginSampleStart,
                                                          segmentStart,
                                                          segmentLength,
                                                          transport);
          }
          if (!sentOk) {
            runtime->hostReady.store(false, std::memory_order_release);
            runtime->active.store(false, std::memory_order_release);
            runtime->needsRestart.store(true, std::memory_order_release);
            break;
          }
          patcherAudioValid = false;
          if (!segment.audioNodeIds.empty()) {
            for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
              outputPtrs[ch] = safeAudioOutPtr(blockIndex, ch);
            }
            const float* const* currentInput = outputPtrs.data();
            for (const auto& nodeInfo : segment.audioNodeIds) {
              if (runAudioPatcherNode(*runtime,
                                      audioGraphSnapshot,
                                      trackState.modLinks,
                                      nodeInfo.nodeId,
                                      nodeInfo.deviceId,
                                      currentInput,
                                      audioModOutputs.data(),
                                      audioModSamples.data())) {
                patcherAudioValid = true;
                currentInput = const_cast<const float* const*>(
                    runtime->patcherAudioChannels.data());
              }
            }
          }
        }

        const uint64_t nextBlockSampleStart =
            sampleStart + static_cast<uint64_t>(engineConfig.blockSize);
        if (routingSnapshot.audioOut.kind == daw::TrackRouteKind::Track) {
          TrackRuntime* dst = findTrackRuntime(routingSnapshot.audioOut.trackId);
          if (dst && dst != runtime) {
            std::vector<const float*> routePtrs;
            const float* const* routeChannels = nullptr;
            if (segments.size() == 1 && segments[0].length == 0) {
              if (patcherAudioValid) {
                routeChannels = const_cast<const float* const*>(
                    runtime->patcherAudioChannels.data());
              } else {
                routeChannels = const_cast<const float* const*>(
                    runtime->inputAudioChannels.data());
              }
            } else {
              routePtrs.resize(engineConfig.numChannelsOut, nullptr);
              for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
                routePtrs[ch] = safeAudioOutPtr(blockIndex, ch);
              }
              routeChannels = routePtrs.data();
            }
            enqueueInboundAudio(*dst, routeChannels);
          }
        }

        if (routingSnapshot.midiOut.kind == daw::TrackRouteKind::Track) {
          TrackRuntime* dst = findTrackRuntime(routingSnapshot.midiOut.trackId);
          if (dst && dst != runtime) {
            enqueueInboundMidi(*dst, routedMidi, sampleStart, nextBlockSampleStart);
          }
        }
      };

      // WHICH TRACKS MAY RUN TOGETHER.
      //
      // Almost everything processTrack touches belongs to its own track: its SHM, its rings,
      // its buffers, its sampler. Two things do not, and they decide this partition.
      //
      // The first is track-to-track ROUTING. A track whose audioOut or midiOut names another
      // track pushes into that track's inbound buffers at the end of its block, and the
      // destination swaps those buffers in at the start of its own. Whether the destination
      // sees this block's audio or next block's therefore depends on which of the two runs
      // first — and the audio accumulates with `+=`, which is not associative in floating
      // point, so even the order of two sources into one destination is audible. Both ENDS of
      // every such route go in the serial group, in exactly the order they have today.
      //
      // The second is the keyjazz PREVIEW path, which allocates note ids from one shared
      // counter. Ids do not change what is heard, but they do change what is logged and
      // matched, and a block with previews is a block where a human just pressed a key — there
      // is nothing to parallelise for. Those blocks run entirely serially.
      //
      // Everything left is isolated by construction: no route reaches it, so no other track's
      // work is observable to it and its own work is observable to no one. Running those
      // together cannot change the result, which is why the parallel and serial paths are
      // checked for BIT-IDENTICAL output rather than merely similar output.
      //
      // (The plugin-editor keystroke drain also writes shared transport state. Only one editor
      // can hold keyboard focus, so only one track's key ring is ever non-empty in a block.)
      std::vector<TrackRuntime*> serialTracks;
      std::vector<TrackRuntime*> parallelTracks;
      {
        std::vector<uint32_t> routeEndpoints;
        for (auto* runtime : trackSnapshot) {
          auto tsPtr = std::atomic_load_explicit(&runtime->trackSnapshot,
                                                 std::memory_order_acquire);
          if (!tsPtr) {
            continue;
          }
          const auto& r = tsPtr->routing;
          if (r.audioOut.kind == daw::TrackRouteKind::Track) {
            routeEndpoints.push_back(runtime->trackId);
            routeEndpoints.push_back(r.audioOut.trackId);
          }
          if (r.midiOut.kind == daw::TrackRouteKind::Track) {
            routeEndpoints.push_back(runtime->trackId);
            routeEndpoints.push_back(r.midiOut.trackId);
          }
        }
        const bool allSerial = !previewThisBlock.empty();
        for (auto* runtime : trackSnapshot) {
          const bool routed =
              std::find(routeEndpoints.begin(), routeEndpoints.end(), runtime->trackId) !=
              routeEndpoints.end();
          if (allSerial || routed) {
            serialTracks.push_back(runtime);
          } else {
            parallelTracks.push_back(runtime);
          }
        }
      }
      // Serial group first, in track order — the order it has always run in. No route touches
      // a parallel-group track, so nothing in the parallel group can observe this.
      for (auto* runtime : serialTracks) {
        processTrack(runtime);
      }
      if (poolAlwaysOn || poolEngaged) {
        renderPool.parallelFor(parallelTracks.size(), [&](std::size_t i) {
          processTrack(parallelTracks[i]);
        });
      } else {
        for (auto* runtime : parallelTracks) {
          processTrack(runtime);
        }
      }

      if (isPlaying) {
        uint64_t nextTicks = blockStartTicks + blockTicks;
        nextTicks = daw::engine::advanceTransportTick(nextTicks, loopStartTicks, loopEndTicks);
        transportNanotick.store(nextTicks, std::memory_order_release);
        // The pass counter moves with the position, by the same amount, before the wrap that
        // throws the pass away. This is the only other place the transport advances.
        transportElapsedNanotick.fetch_add(blockTicks, std::memory_order_acq_rel);
      }
      // Fold this block into the producer-load counters, BEFORE the throttle sleep below —
      // that sleep is deliberate pacing, not work, and folding it in would report an idling
      // producer as a saturated one. Only while PLAYING: a stopped transport still walks this
      // loop to publish UI state, and those blocks have no deadline to measure against.
      if (isPlaying) {
        const uint64_t blockUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - blockWorkStart)
                .count());
        producerBlocksTimed.fetch_add(1, std::memory_order_relaxed);
        producerBlockUsTotal.fetch_add(blockUs, std::memory_order_relaxed);
        const uint64_t samplerUs = blockSamplerUs.load(std::memory_order_relaxed);
        producerSamplerUsTotal.fetch_add(samplerUs, std::memory_order_relaxed);
        if (blockUs > producerBlockUsMax.load(std::memory_order_relaxed)) {
          producerBlockUsMax.store(blockUs, std::memory_order_relaxed);
        }
        if (samplerUs > producerSamplerUsMax.load(std::memory_order_relaxed)) {
          producerSamplerUsMax.store(samplerUs, std::memory_order_relaxed);
        }
        // ENGAGE OR DISENGAGE, with HYSTERESIS so a project sitting near the line does not
        // change mode every block — switching costs a thread wake-up per block, which is the
        // very cost being avoided. Thresholds are fractions of the block deadline: at 8 sampler
        // tracks the work is ~0.20x and stays serial, at 32 it is ~0.75x and the pool takes it.
        poolWorkEwmaUs = poolWorkEwmaUs * 0.9 + static_cast<double>(samplerUs) * 0.1;
        if (producerBlockBudgetUs > 0) {
          const double frac =
              poolWorkEwmaUs / static_cast<double>(producerBlockBudgetUs);
          if (!poolEngaged && frac > 0.35) {
            poolEngaged = true;
            DAW_EVENT("producer.pool_engaged")
                .field("work_us", static_cast<uint64_t>(poolWorkEwmaUs))
                .field("budget_us", producerBlockBudgetUs);
          } else if (poolEngaged && frac < 0.25) {
            poolEngaged = false;
            DAW_EVENT("producer.pool_disengaged")
                .field("work_us", static_cast<uint64_t>(poolWorkEwmaUs))
                .field("budget_us", producerBlockBudgetUs);
          }
        }
        if (blockUs > producerBlockBudgetUs) {
          producerBlocksOverBudget.fetch_add(1, std::memory_order_relaxed);
          // WHICH block, and how much of it was the sampler. A mean is reassuring and a peak
          // is not actionable without this: "8 sampler tracks peaked at 2.5x budget" could be
          // steady-state DSP that a render pool fixes, or the one startup block that decodes
          // samples, or a UI snapshot publish that no amount of DSP threading touches. The
          // answer changes what you build, so it is recorded rather than guessed at.
          //
          // Rate-limited to the first 32: a genuinely saturated producer would otherwise log
          // once per block forever, and the log itself becomes the load.
          if (producerBlocksOverBudget.load(std::memory_order_relaxed) <= 32) {
            DAW_EVENT("producer.over_budget")
                .field("block", static_cast<uint64_t>(blockId))
                .field("us", blockUs)
                .field("sampler_us", samplerUs)
                .field("budget_us", producerBlockBudgetUs);
          }
        }
      }
      if (throttleInactive || throttlePlayback) {
        std::this_thread::sleep_for(blockDuration);
      }
}

}  // namespace daw::engine
