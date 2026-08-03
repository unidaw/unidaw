// Bodies for apps/engine_render_track.h. The body below is the lambda that lived in
// main(), moved WITHOUT EDITS: the references bound at the top carry the names it
// captured, so nested lambdas, shadowing and the brace scope all still mean what they
// meant. Rewriting 1,300 lines to say `deps.x` would have touched every one of them.
#include "apps/engine_render_track.h"
#include "apps/engine_producer_helpers.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <array>
#include <map>
#include <memory>
#include <algorithm>
#include <tuple>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <limits>
#include <unordered_map>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "platform_juce/juce_wrapper.h"
#include "apps/audio_shm.h"
#include "apps/engine_instance.h"
#include "apps/engine_types.h"
#include "apps/engine_pure.h"
#include "apps/engine_rt_helpers.h"
#include "apps/engine_automation_commands.h"
#include "apps/engine_clip_commands.h"
#include "apps/engine_modlink_commands.h"
#include "apps/engine_module_commands.h"
#include "apps/engine_patcher_commands.h"
#include "apps/engine_chain_commands.h"
#include "apps/engine_marker_commands.h"
#include "apps/engine_project_commands.h"
#include "apps/engine_rowops_commands.h"
#include "apps/engine_track_commands.h"
#include "apps/engine_request_commands.h"
#include "apps/engine_trackprops_commands.h"
#include "apps/engine_device_commands.h"
#include "apps/engine_note_commands.h"
#include "apps/engine_undo_commands.h"
#include "apps/engine_sampler_commands.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/rt_thread.h"
#include "apps/render_pool.h"
#include "apps/host_controller.h"
#include "apps/plugin_cache.h"
#include "apps/patcher_abi.h"
#include "apps/audio_region.h"
#include "apps/clip_grid.h"
#include "apps/waveform_store.h"
#include "apps/patcher_assemble.h"
#include "apps/patcher_graph.h"
#include "apps/patcher_preset.h"
#include "apps/patcher_preset_library.h"
#include "apps/event_log.h"
#include "apps/project_file.h"
#include "apps/device_chain.h"
#include "apps/modulation.h"
#include "apps/track_routing.h"
#include "apps/watchdog.h"
#include "apps/latency_manager.h"
#include "apps/time_base.h"
#include "apps/lane_quantize.h"
#include "apps/markers.h"
#include "apps/ripple.h"
#include "apps/sampler_engine.h"
#include "apps/sampler_slice.h"
#include "apps/musical_structures.h"
#include "apps/placement_schedule.h"
#include "apps/note_entry.h"
#include "apps/placement_flatten.h"
#include "apps/automation_clip.h"
#include "apps/uid_hash.h"
#include "apps/scale_library.h"
#include "apps/harmony_timeline.h"
#include "apps/chord_resolver.h"
#include "apps/ui_snapshot.h"
#include "apps/clip_edit.h"

namespace daw::engine {

bool renderTrack(RenderTrackDeps& deps,
                 TrackRuntime& runtime,
                 const TrackStateSnapshot& trackState,
                 uint64_t windowStartTicks,
                 uint64_t windowEndTicks,
                 uint64_t blockSampleStart,
                 uint32_t currentBlockId,
                 daw::EventRingView& ringStd,
                 std::vector<daw::EventEntry>* routedMidi,
                 uint64_t blockTicks,
                 uint64_t loopStartTicks,
                 uint64_t loopEndTicks,
                 uint64_t loopLen) {
  auto& engineConfig = deps.engineConfig;
  auto& harmonyEvents = deps.harmonyEvents;
  auto& harmonyMutex = deps.harmonyMutex;
  auto& lastOverflowTick = deps.lastOverflowTick;
  auto& latencyMgr = deps.latencyMgr;
  auto& nextNoteId = deps.nextNoteId;
  auto& patcherGraphSnapshot = deps.patcherGraphSnapshot;
  auto& patcherParallel = deps.patcherParallel;
  auto& patcherPool = deps.patcherPool;
  auto& projectSeed = deps.projectSeed;
  auto& tempoProvider = deps.tempoProvider;
  auto& traceNotes = deps.traceNotes;
  auto& transportElapsedNanotick = deps.transportElapsedNanotick;
  auto& warnedEventOutsideBlock = deps.warnedEventOutsideBlock;
  const auto& getHarmonyAt = deps.getHarmonyAt;
  const auto& getScaleForHarmony = deps.getScaleForHarmony;
  const auto& quantizePitch = deps.quantizePitch;
  const auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
  const auto& wrapTick = deps.wrapTick;
  {
        // Movement 4 MIDI-per-bus: an aux child's notes are tagged with its bus's MIDI
        // channel and rendered into the PARENT's ring (the caller passes the parent's
        // ringStd), so a multitimbral instrument routes channel k to its output bus k.
        // A normal track uses channel 0.
        const uint8_t midiChannel =
            runtime.isAuxChild.load(std::memory_order_relaxed)
                ? static_cast<uint8_t>(
                      runtime.auxBusIndex.load(std::memory_order_relaxed) & 0x0Fu)
                : 0u;
        const long double bpm = tempoProvider.bpmAtNanotick(windowStartTicks);
        const long double safeBpm = bpm > 0.0 ? bpm : 120.0;
        const long double ticksPerQuarter =
            static_cast<long double>(daw::NanotickConverter::kNanoticksPerQuarter);
        const long double samplesPerTick =
            (static_cast<long double>(engineConfig.sampleRate) * 60.0L) /
            (safeBpm * ticksPerQuarter);
        // Binds the block's rate; the rounding rule is in apps/engine_rt_helpers.h with a test.
        auto tickDeltaToSamples = [&](uint64_t tickDelta) -> uint64_t {
          return daw::engine::tickDeltaToSamples(tickDelta, samplesPerTick);
        };
        auto removeNoteIdFromColumn = [&](uint8_t column, uint32_t noteId) {
          daw::engine::removeNoteIdFromColumn(runtime, column, noteId);
        };
        auto& scratchpad = runtime.patcherScratchpad;
        if (scratchpad.size() < kPatcherScratchpadCapacity) {
          scratchpad.resize(kPatcherScratchpadCapacity);
        }
        uint32_t scratchpadCount = 0;
        // The state the note-cutting rule needs, named once. Plain references: this is producer-
        // thread code, so no std::function and no indirection per block.
        daw::engine::NoteCutCtx noteCutCtx{runtime, scratchpadCount, lastOverflowTick,
                                           midiChannel, blockSampleStart,
                                           engineConfig.blockSize};
        auto pushScratchpad = [&](const daw::EventEntry& entry,
                                  uint64_t overflowTick) -> bool {
          return daw::engine::pushScratchpad(noteCutCtx, entry, overflowTick);
        };
        const uint64_t blockSampleEnd =
            blockSampleStart + static_cast<uint64_t>(engineConfig.blockSize);
        auto& inboundEvents = runtime.inboundMidiScratch;
        {
          std::lock_guard<std::mutex> lock(runtime.inboundMutex);
          runtime.inboundMidiEvents.swap(inboundEvents);
          runtime.inboundMidiEvents.clear();
        }
        if (!inboundEvents.empty()) {
          for (const auto& entry : inboundEvents) {
            if (entry.sampleTime < blockSampleStart ||
                entry.sampleTime >= blockSampleEnd) {
              continue;
            }
            int64_t offsetSamples =
                static_cast<int64_t>(entry.sampleTime) -
                static_cast<int64_t>(blockSampleStart);
            // CLAMPED INTO THE BLOCK, NOT DROPPED.
            //
            // Everything in this scratchpad was generated FOR this block's TICK window, so it
            // belongs to this block by construction. Its sample time is a CONVERSION of that
            // tick, and a conversion can land a sample outside: at 120 bpm and 44.1 kHz the
            // sixteenth at 1.875 s sits at sample 82687.5, so a block covering [82432, 82688)
            // converts it to 82688 — the first sample of the NEXT block. This test dropped it
            // there, and the next block never emitted it either, because its TICK window starts
            // after that step. The note simply vanished.
            //
            // It bites only when a step lands almost exactly on a block boundary, so WHICH notes
            // vanish depends on the buffer size: at 256 frames that sixteenth is on a boundary
            // and is lost, at 1024 it is not and it plays. One missing note in an eight-second
            // render at one buffer size — which is why it survived until a check demanded that
            // two renders be BIT-IDENTICAL rather than merely similar.
            //
            // Half a sample early is inaudible; a missing note is not. Widening the window
            // instead would let the same event be emitted by two consecutive blocks, which is a
            // doubled note rather than a missing one — no better.
            if (offsetSamples < 0 ||
                offsetSamples >= static_cast<int64_t>(engineConfig.blockSize)) {
              // SAID OUT LOUD, ONCE. With the generator's floor conversion in place this cannot
              // fire — its own negative control passes, which is the honest way to describe a
              // guard that no longer has a reproducer. It is kept because the CLIP path converts
              // ticks to samples by a different route that has not been audited for the same
              // property, and because the alternative behaviour here was to DELETE the note.
              //
              // If it ever does fire, this line is the difference between a diagnosable report
              // and another year of "a note goes missing sometimes".
              if (!warnedEventOutsideBlock.exchange(true, std::memory_order_relaxed)) {
                DAW_EVENT("patcher.event_outside_block")
                    .field("offset", offsetSamples)
                    .field("block_size", static_cast<uint32_t>(engineConfig.blockSize));
              }
              offsetSamples = offsetSamples < 0
                                  ? 0
                                  : static_cast<int64_t>(engineConfig.blockSize) - 1;
            }
            const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                static_cast<long double>(offsetSamples) / samplesPerTick));
            const uint64_t eventTick = wrapTick(windowStartTicks + tickDelta);
            pushScratchpad(entry, eventTick);
          }
        }
        static const daw::PatcherGraph kEmptyGraph{};
        auto graphPtr = std::atomic_load_explicit(&patcherGraphSnapshot,
                                                  std::memory_order_acquire);
        const daw::PatcherGraph& graphSnapshot =
            graphPtr ? *graphPtr : kEmptyGraph;
        const uint32_t nodeCount =
            static_cast<uint32_t>(graphSnapshot.nodes.size());
        auto& nodeBuffers = runtime.patcherNodeBuffers;
        auto& nodeModOutputs = runtime.patcherNodeModOutputs;
        if (nodeBuffers.size() < nodeCount) {
          nodeBuffers.resize(nodeCount);
        }
        if (nodeModOutputs.size() < nodeCount) {
          nodeModOutputs.resize(nodeCount);
        }
        auto& modOutputSamples = runtime.patcherModOutputSamples;
        auto& modInputSamples = runtime.patcherModInputSamples;
        auto& modUpdates = runtime.patcherModUpdates;
        if (nodeCount > 0) {
          const size_t sampleCount =
              static_cast<size_t>(nodeCount) *
              static_cast<size_t>(kPatcherMaxModOutputs) *
              static_cast<size_t>(engineConfig.blockSize);
          if (modOutputSamples.size() != sampleCount) {
            modOutputSamples.assign(sampleCount, 0.0f);
          } else {
            std::fill(modOutputSamples.begin(), modOutputSamples.end(), 0.0f);
          }
          if (modInputSamples.size() != sampleCount) {
            modInputSamples.assign(sampleCount, 0.0f);
          } else {
            std::fill(modInputSamples.begin(), modInputSamples.end(), 0.0f);
          }
        } else {
          modOutputSamples.clear();
          modInputSamples.clear();
        }
        modUpdates.clear();
        if (modUpdates.capacity() < static_cast<size_t>(nodeCount) * kPatcherMaxModOutputs) {
          modUpdates.reserve(static_cast<size_t>(nodeCount) * kPatcherMaxModOutputs);
        }
        if (nodeCount > 0) {
          if (runtime.modOutputSamples.size() != modOutputSamples.size()) {
            runtime.modOutputSamples.resize(modOutputSamples.size());
          }
          std::fill(runtime.modOutputSamples.begin(),
                    runtime.modOutputSamples.end(),
                    0.0f);
          if (runtime.modOutputDeviceIds.size() != nodeCount) {
            runtime.modOutputDeviceIds.resize(nodeCount);
          }
          std::fill(runtime.modOutputDeviceIds.begin(),
                    runtime.modOutputDeviceIds.end(),
                    daw::kDeviceIdAuto);
        } else {
          runtime.modOutputSamples.clear();
          runtime.modOutputDeviceIds.clear();
        }
        auto& nodeAllowed = runtime.patcherNodeAllowed;
        auto& nodeSeen = runtime.patcherNodeSeen;
        auto& nodeStack = runtime.patcherNodeStack;
        auto& chainOrder = runtime.patcherChainOrder;
        auto& nodeToDeviceId = runtime.patcherNodeToDeviceId;
        auto& modLinks = runtime.patcherModLinks;
        auto nodeIndexForId = [&](uint32_t nodeId) -> std::optional<uint32_t> {
          return daw::engine::nodeIndexForId(graphSnapshot, nodeId);
        };
        chainOrder.clear();
        bool useNodeFilter = false;
        if (nodeToDeviceId.size() != nodeCount) {
          nodeToDeviceId.resize(nodeCount);
        }
        std::fill(nodeToDeviceId.begin(), nodeToDeviceId.end(), daw::kDeviceIdAuto);
        if (modLinks.capacity() < trackState.modLinks.size()) {
          modLinks.reserve(trackState.modLinks.size());
        }
        modLinks.assign(trackState.modLinks.begin(), trackState.modLinks.end());
        for (const auto& device : trackState.chainDevices) {
          if (device.bypass) {
            continue;
          }
          if (device.kind == daw::DeviceKind::PatcherEvent ||
              device.kind == daw::DeviceKind::PatcherInstrument ||
              device.kind == daw::DeviceKind::PatcherAudio) {
            useNodeFilter = true;
            break;
          }
        }
        if (useNodeFilter) {
          if (nodeAllowed.size() != nodeCount) {
            nodeAllowed.resize(nodeCount);
          }
          std::fill(nodeAllowed.begin(), nodeAllowed.end(), false);
          if (nodeSeen.size() != nodeCount) {
            nodeSeen.resize(nodeCount);
          }
          std::fill(nodeSeen.begin(), nodeSeen.end(), false);
          nodeStack.clear();
          if (nodeStack.capacity() < nodeCount) {
            nodeStack.reserve(nodeCount);
          }
          for (const auto& device : trackState.chainDevices) {
            if (device.bypass) {
              continue;
            }
            if (device.kind == daw::DeviceKind::PatcherEvent ||
                device.kind == daw::DeviceKind::PatcherInstrument ||
                device.kind == daw::DeviceKind::PatcherAudio) {
              if (auto nodeIndex = nodeIndexForId(device.patcherNodeId)) {
                nodeAllowed[*nodeIndex] = true;
                if (nodeToDeviceId[*nodeIndex] == daw::kDeviceIdAuto) {
                  nodeToDeviceId[*nodeIndex] = device.id;
                }
                if (!nodeSeen[*nodeIndex]) {
                  chainOrder.push_back(*nodeIndex);
                  nodeSeen[*nodeIndex] = true;
                }
              }
            }
          }
          for (uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            if (!nodeAllowed[nodeIndex]) {
              continue;
            }
            nodeStack.push_back(nodeIndex);
            while (!nodeStack.empty()) {
              const uint32_t current = nodeStack.back();
              nodeStack.pop_back();
              if (current >= graphSnapshot.nodes.size()) {
                continue;
              }
              for (uint32_t inputIndex : graphSnapshot.resolvedInputs[current]) {
                if (inputIndex < nodeCount && !nodeAllowed[inputIndex]) {
                  nodeAllowed[inputIndex] = true;
                  nodeStack.push_back(inputIndex);
                }
              }
            }
          }
        }
        if (!useNodeFilter) {
          for (const auto& device : trackState.chainDevices) {
            if (device.bypass) {
              continue;
            }
            if (device.kind == daw::DeviceKind::PatcherEvent ||
                device.kind == daw::DeviceKind::PatcherInstrument ||
                device.kind == daw::DeviceKind::PatcherAudio) {
              if (auto nodeIndex = nodeIndexForId(device.patcherNodeId)) {
                if (nodeToDeviceId[*nodeIndex] == daw::kDeviceIdAuto) {
                  nodeToDeviceId[*nodeIndex] = device.id;
                }
              }
            }
          }
        }
        auto& euclidOverrides = runtime.patcherEuclidOverrides;
        auto& hasEuclidOverride = runtime.patcherHasEuclidOverride;
        if (euclidOverrides.size() != nodeCount) {
          euclidOverrides.resize(nodeCount);
        }
        std::fill(euclidOverrides.begin(),
                  euclidOverrides.end(),
                  daw::PatcherEuclideanConfig{});
        if (hasEuclidOverride.size() != nodeCount) {
          hasEuclidOverride.resize(nodeCount);
        }
        std::fill(hasEuclidOverride.begin(),
                  hasEuclidOverride.end(),
                  false);
        for (const auto& device : trackState.chainDevices) {
          if (device.bypass) {
            continue;
          }
          if (!device.hasEuclideanConfig) {
            continue;
          }
          auto nodeIndex = nodeIndexForId(device.patcherNodeId);
          if (!nodeIndex) {
            continue;
          }
          if (graphSnapshot.nodes[*nodeIndex].type !=
              daw::PatcherNodeType::Euclidean) {
            continue;
          }
          euclidOverrides[*nodeIndex] = device.euclideanConfig;
          hasEuclidOverride[*nodeIndex] = true;
        }
        const uint16_t maxDepth = graphSnapshot.maxDepth;
        auto mergeNodeBuffers = [&]() {
          for (uint32_t orderIndex = 0;
               orderIndex < graphSnapshot.topoOrder.size();
               ++orderIndex) {
            const uint32_t nodeIndex = graphSnapshot.topoOrder[orderIndex];
            const auto& buffer = nodeBuffers[nodeIndex];
            for (uint32_t i = 0; i < buffer.count; ++i) {
              const auto& entry = buffer.events[i];
              const int64_t offsetSamples =
                  static_cast<int64_t>(entry.sampleTime) -
                  static_cast<int64_t>(blockSampleStart);
              uint64_t overflowTick = windowStartTicks;
              if (offsetSamples >= 0) {
                const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                    static_cast<long double>(offsetSamples) / samplesPerTick));
                overflowTick = wrapTick(windowStartTicks + tickDelta);
              }
              pushScratchpad(entry, overflowTick);
            }
          }
        };
        std::array<daw::HarmonyEvent, daw::kUiMaxHarmonyEvents> harmonySnapshot{};
        uint32_t harmonyCount = 0;
        {
          std::lock_guard<std::mutex> lock(harmonyMutex);
          harmonyCount = static_cast<uint32_t>(
              std::min<size_t>(harmonyEvents.size(), harmonySnapshot.size()));
          for (uint32_t i = 0; i < harmonyCount; ++i) {
            harmonySnapshot[i] = harmonyEvents[i];
          }
        }
        uint32_t paramTargetIndex = daw::kParamTargetAll;
        uint32_t hostIndex = 0;
        for (const auto& device : trackState.chainDevices) {
          if (device.kind != daw::DeviceKind::VstInstrument &&
              device.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          if (device.bypass) {
            continue;
          }
          if (resolveDevicePluginPath(runtime, device.hostSlotIndex)) {
            paramTargetIndex = hostIndex;
            break;
          }
          hostIndex++;
        }
        std::atomic<bool> patcherAudioWritten{false};
        auto runNode = [&](uint32_t nodeIndex) {
          if (nodeIndex >= nodeCount) {
            return;
          }
          if (useNodeFilter && (nodeIndex >= nodeAllowed.size() ||
                                !nodeAllowed[nodeIndex])) {
            return;
          }
          const auto& node = graphSnapshot.nodes[nodeIndex];
          auto& buffer = nodeBuffers[nodeIndex];
          buffer.count = 0;
          for (uint32_t inputIndex : graphSnapshot.resolvedInputs[nodeIndex]) {
            if (inputIndex >= nodeCount) {
              continue;
            }
            const auto& inputBuffer = nodeBuffers[inputIndex];
            for (uint32_t i = 0; i < inputBuffer.count; ++i) {
              if (buffer.count < buffer.events.size()) {
                buffer.events[buffer.count++] = inputBuffer.events[i];
              } else {
                daw::atomic_store_u64(
                    reinterpret_cast<uint64_t*>(&lastOverflowTick),
                    windowStartTicks);
                break;
              }
            }
          }
          daw::PatcherContext ctx{};
          ctx.abi_version = daw::kPatcherAbiVersion;
          ctx.node_id = node.id;
          ctx.seed = projectSeed.load(std::memory_order_relaxed);
          ctx.block_start_tick = windowStartTicks;
          ctx.block_end_tick = windowEndTicks;
          ctx.block_start_sample = blockSampleStart;
          ctx.sample_rate = static_cast<float>(engineConfig.sampleRate);
          const double bpm = tempoProvider.bpmAtNanotick(windowStartTicks);
          ctx.tempo_bpm = static_cast<float>(bpm > 0.0 ? bpm : 120.0);
          ctx.num_frames = engineConfig.blockSize;
          ctx.event_buffer = buffer.events.data();
          ctx.event_capacity = static_cast<uint32_t>(buffer.events.size());
          ctx.event_count = &buffer.count;
          ctx.last_overflow_tick =
              reinterpret_cast<uint64_t*>(&lastOverflowTick);
          ctx.audio_channels = nullptr;
          ctx.num_channels = 0;
          auto& modOut = nodeModOutputs[nodeIndex];
          std::fill(modOut.begin(), modOut.end(), 0.0f);
          ctx.mod_outputs = modOut.data();
          ctx.mod_output_count = kPatcherMaxModOutputs;
          ctx.mod_output_samples = nullptr;
          ctx.mod_output_stride = 0;
          if (!modOutputSamples.empty()) {
            ctx.mod_output_samples =
                modOutputSamples.data() +
                static_cast<size_t>(nodeIndex) *
                    static_cast<size_t>(kPatcherMaxModOutputs) *
                    static_cast<size_t>(engineConfig.blockSize);
            ctx.mod_output_stride = engineConfig.blockSize;
          }
          ctx.mod_inputs = nullptr;
          ctx.mod_input_count = 0;
          ctx.mod_input_stride = 0;
          if (!modInputSamples.empty()) {
            ctx.mod_inputs = modInputSamples.data() +
                static_cast<size_t>(nodeIndex) *
                    static_cast<size_t>(kPatcherMaxModOutputs) *
                    static_cast<size_t>(engineConfig.blockSize);
            ctx.mod_input_count = kPatcherMaxModOutputs;
            ctx.mod_input_stride = engineConfig.blockSize;
            const size_t stride = static_cast<size_t>(engineConfig.blockSize);
            std::fill(ctx.mod_inputs,
                      ctx.mod_inputs +
                          static_cast<size_t>(kPatcherMaxModOutputs) * stride,
                      0.0f);
          }
          ctx.node_config = nullptr;
          ctx.node_config_size = 0;
          if (node.type == daw::PatcherNodeType::Euclidean) {
            if (nodeIndex < hasEuclidOverride.size() && hasEuclidOverride[nodeIndex]) {
              ctx.node_config = &euclidOverrides[nodeIndex];
              ctx.node_config_size = sizeof(daw::PatcherEuclideanConfig);
            } else if (node.hasEuclideanConfig) {
              ctx.node_config = &node.euclideanConfig;
              ctx.node_config_size = sizeof(node.euclideanConfig);
            }
          } else if (node.type == daw::PatcherNodeType::RandomDegree) {
            if (node.hasRandomDegreeConfig) {
              ctx.node_config = &node.randomDegreeConfig;
              ctx.node_config_size = sizeof(node.randomDegreeConfig);
            }
          } else if (node.type == daw::PatcherNodeType::SliceSelect) {
            if (node.hasSliceSelectConfig) {
              ctx.node_config = &node.sliceSelectConfig;
              ctx.node_config_size = sizeof(node.sliceSelectConfig);
            }
          } else if (node.type == daw::PatcherNodeType::Lfo) {
            if (node.hasLfoConfig) {
              ctx.node_config = &node.lfoConfig;
              ctx.node_config_size = sizeof(node.lfoConfig);
            }
          }
          if (node.type == daw::PatcherNodeType::AudioPassthrough) {
            const uint32_t channels = engineConfig.numChannelsOut;
            if (runtime.patcherAudioChannels.size() != channels) {
              runtime.patcherAudioChannels.resize(channels);
            }
            if (runtime.patcherAudioBuffer.size() !=
                static_cast<size_t>(channels) * engineConfig.blockSize) {
              runtime.patcherAudioBuffer.assign(
                  static_cast<size_t>(channels) * engineConfig.blockSize, 0.0f);
            } else {
              std::fill(runtime.patcherAudioBuffer.begin(),
                        runtime.patcherAudioBuffer.end(), 0.0f);
            }
            for (uint32_t ch = 0; ch < channels; ++ch) {
              runtime.patcherAudioChannels[ch] =
                  runtime.patcherAudioBuffer.data() +
                  static_cast<size_t>(ch) * engineConfig.blockSize;
            }
            ctx.audio_channels = runtime.patcherAudioChannels.data();
            ctx.num_channels = channels;
            patcherAudioWritten.store(true, std::memory_order_relaxed);
          }
          ctx.harmony_snapshot = harmonySnapshot.data();
          ctx.harmony_count = harmonyCount;
          if (ctx.mod_inputs && !modLinks.empty()) {
            const uint32_t deviceId =
                nodeIndex < nodeToDeviceId.size()
                    ? nodeToDeviceId[nodeIndex]
                    : daw::kDeviceIdAuto;
            if (deviceId != daw::kDeviceIdAuto) {
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
                for (uint32_t i = 0; i < nodeToDeviceId.size(); ++i) {
                  if (nodeToDeviceId[i] == link.source.deviceId) {
                    sourceIndex = i;
                    break;
                  }
                }
                if (sourceIndex == daw::kDeviceIdAuto ||
                    modOutputSamples.empty()) {
                  continue;
                }
                const size_t stride =
                    static_cast<size_t>(engineConfig.blockSize);
                const size_t sourceBase =
                    (static_cast<size_t>(sourceIndex) *
                         static_cast<size_t>(kPatcherMaxModOutputs) +
                     link.source.sourceId) *
                    stride;
                const size_t targetBase =
                    (static_cast<size_t>(link.target.targetId)) * stride;
                const float* source = modOutputSamples.data() + sourceBase;
                float* target = ctx.mod_inputs + targetBase;
                for (size_t i = 0; i < stride; ++i) {
                  target[i] += link.bias + link.depth * source[i];
                }
              }
            }
          }
          dispatchRustKernel(node.type, ctx);
          const uint32_t deviceId =
              nodeIndex < nodeToDeviceId.size()
                  ? nodeToDeviceId[nodeIndex]
                  : daw::kDeviceIdAuto;
          if (deviceId != daw::kDeviceIdAuto) {
            for (uint32_t i = 0; i < ctx.mod_output_count; ++i) {
              daw::ModSourceState state{};
              state.ref.deviceId = deviceId;
              state.ref.sourceId = i;
              state.ref.kind = daw::ModSourceKind::PatcherNodeOutput;
              state.value = modOut[i];
              modUpdates.push_back(state);
            }
          }
        };

        if (useNodeFilter && !chainOrder.empty()) {
          std::vector<uint8_t> visitState(nodeCount, 0);
          std::vector<uint32_t> stack;
          std::vector<uint32_t> nodeIter;
          stack.reserve(nodeCount);
          nodeIter.reserve(nodeCount);
          auto runNodeWithDeps = [&](uint32_t startNode) {
            stack.push_back(startNode);
            while (!stack.empty()) {
              const uint32_t current = stack.back();
              if (current >= nodeCount) {
                stack.pop_back();
                continue;
              }
              const uint8_t state = visitState[current];
              if (state == 2) {
                stack.pop_back();
                continue;
              }
              if (state == 1) {
                visitState[current] = 2;
                stack.pop_back();
                runNode(current);
                continue;
              }
              visitState[current] = 1;
              const auto& inputs = graphSnapshot.resolvedInputs[current];
              for (auto it = inputs.rbegin(); it != inputs.rend(); ++it) {
                const uint32_t input = *it;
                if (input < nodeCount && visitState[input] == 0) {
                  stack.push_back(input);
                }
              }
            }
          };
          for (uint32_t nodeIndex : chainOrder) {
            runNodeWithDeps(nodeIndex);
          }
        } else {
          for (uint16_t depth = 0; depth <= maxDepth; ++depth) {
            std::vector<uint32_t> depthNodes;
            for (uint32_t i = 0; i < nodeCount; ++i) {
              if (graphSnapshot.depths[i] == depth) {
                depthNodes.push_back(i);
              }
            }
            if (patcherParallel && depthNodes.size() > 1 && patcherPool) {
              for (uint32_t nodeIndex : depthNodes) {
                patcherPool->enqueue([&, nodeIndex]() { runNode(nodeIndex); });
              }
              patcherPool->wait();
            } else {
              for (uint32_t nodeIndex : depthNodes) {
                runNode(nodeIndex);
              }
            }
          }
        }

        if (!modOutputSamples.empty() &&
            runtime.modOutputSamples.size() == modOutputSamples.size()) {
          std::memcpy(runtime.modOutputSamples.data(),
                      modOutputSamples.data(),
                      modOutputSamples.size() * sizeof(float));
        }
        if (!nodeToDeviceId.empty() &&
            runtime.modOutputDeviceIds.size() == nodeToDeviceId.size()) {
          runtime.modOutputDeviceIds = nodeToDeviceId;
        }

        mergeNodeBuffers();
        auto emitAutomationPoints = [&](const daw::AutomationClip& automationClip,
                                        uint64_t rangeStart,
                                        uint64_t rangeEnd,
                                        uint64_t baseTickDelta,
                                        const std::array<uint8_t, 16>& uid16) {
          uint32_t targetIndex = automationClip.targetPluginIndex();
          if (targetIndex == daw::kParamTargetAll) {
            targetIndex = paramTargetIndex;
          }
          std::vector<const daw::AutomationPoint*> points;
          automationClip.getPointsInRange(rangeStart, rangeEnd, points);
          for (const auto* point : points) {
            const uint64_t tickDelta =
                baseTickDelta + (point->nanotick - rangeStart);
            const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                         samplesPerTick, engineConfig.blockSize);
            if (!placed) {
              continue;
            }
            const uint64_t eventSample = placed->sampleTime;
            daw::EventEntry paramEntry;
            paramEntry.sampleTime = eventSample;
            paramEntry.blockId = 0;
            paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
            paramEntry.size = sizeof(daw::ParamPayload);
            daw::ParamPayload payload{};
            std::memcpy(payload.uid16, uid16.data(), uid16.size());
            payload.value = point->value;
            payload.targetPluginIndex = targetIndex;
            std::memcpy(paramEntry.payload, &payload, sizeof(payload));
            {
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              runtime.paramMirror[uid16] = ParamMirrorEntry{point->value, targetIndex};
            }
            pushScratchpad(paramEntry, point->nanotick);
          }
        };
        auto emitNotes = [&](uint64_t rangeStart,
                             uint64_t rangeEnd,
                             uint64_t baseTickDelta) {
          auto cutActiveNoteInColumn = [&](uint8_t column,
                                           uint64_t eventSample,
                                           uint32_t currentBlockId) {
            (void)currentBlockId;
            daw::engine::cutActiveNotes(noteCutCtx, eventSample, column);
          };


          // Emit a note-on at onTick (assumed within this window) and schedule
          // its note-off — in-block if it lands here, else via activeNotes for a
          // later block. Shared by the plain note path, the row-op strike path,
          // and the pending-strike drain, so all three emit identically. Must be
          // called without activeNotesMutex held (it takes the lock itself).
          auto emitNoteOnWithOff = [&](uint64_t onTick, uint64_t duration,
                                       uint8_t pitch, uint8_t velocity,
                                       uint8_t noteColumn, float noteTuningCents,
                                       uint16_t sound = 0, uint16_t soundOffset = 0) {
            const uint64_t tickDelta = baseTickDelta + (onTick - rangeStart);
            const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                         samplesPerTick, engineConfig.blockSize);
            if (!placed) {
              return;
            }
            const uint64_t eventSample = placed->sampleTime;
            const int64_t offset = static_cast<int64_t>(placed->offsetInBlock);
            const uint32_t noteId =
                nextNoteId.fetch_add(1, std::memory_order_acq_rel);
            const daw::EventEntry midiEntry = daw::engine::makeNoteOnEntry(
                eventSample, 0, pitch, velocity, midiChannel, noteTuningCents, noteId);
            pushScratchpad(midiEntry, onTick);
            // TEE TO THE BUILT-IN SAMPLER at the exact frame within this block.
            //
            // This comment used to say the hosted-plugin path computes the offset "and then
            // throws away". Measured 2026-08-03: it does not. juce_host_process derives
            // sampleOffset from sampleTime - blockStart and JUCE honours it, so a note at
            // 5512.5 samples lands on 5513, not on a block boundary. Both paths are
            // sample-accurate; see docs/SAMPLER_DESIGN.md §3.5 for the render that settled it.
            if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
runtime.samplerEvents.push_back(daw::engine::samplerNoteOnFor(
                  static_cast<uint32_t>(offset), pitch, velocity, noteColumn, sound,
                  soundOffset, trackState.soundAddressedOnly, noteId));
            }
            if (traceNotes) {
              DAW_EVENT("note.emit")
                  .field("track", runtime.trackId)
                  .field("tick", onTick)
                  .field("pitch", static_cast<uint64_t>(pitch))
                  .field("dur", duration);
            }

            if (duration == 0) {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              daw::engine::registerActiveNote(runtime, noteId, pitch, noteColumn,
                                              onTick, onTick, noteTuningCents, false);
              return;
            }
            const uint64_t noteEndTick = onTick + duration;
            // A NOTE ENDING EXACTLY ON THE LOOP POINT MUST NOT WRAP ONTO ITS OWN START.
            //
            // wrapTick maps loopEnd to loopStart, which is right for a POSITION and wrong for an
            // END: a note filling the whole pattern has onTick == loopStart and noteEndTick ==
            // loopEnd, so its note-off wrapped to loopStart — the same tick as its note-on — and
            // the voice was cut the instant it started. A gated slot honours note-off, so "a pad
            // note filling the bar" rendered SILENT with every structural fact correct: the note
            // is in the clip, it emits, the slot resolves. A one-shot slot ignores note-off and
            // was therefore fine, which is why this hid.
            //
            // Nudged one tick earlier rather than left at the boundary: leaving it AT loopEnd
            // means no block's half-open window contains it and the note never releases at all —
            // a stuck note instead of a silent one, which is not an improvement. One nanotick is
            // 1/960000 of a quarter.
            uint64_t offTick = wrapTick(noteEndTick);
            if (duration > 0 && loopLen != 0 && noteEndTick >= loopEndTicks &&
                offTick == wrapTick(onTick)) {
              offTick = loopEndTicks - 1;
            }
            if (offTick >= rangeStart && offTick < rangeEnd) {
              const uint64_t offDelta = baseTickDelta + (offTick - rangeStart);
              const uint64_t offSample =
                  blockSampleStart + tickDeltaToSamples(offDelta);
              const int64_t offOffset = static_cast<int64_t>(offSample) -
                                        static_cast<int64_t>(blockSampleStart);
              if (offOffset >= 0 &&
                  offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                    offSample, 0, pitch,
                    midiChannel, noteTuningCents, noteId);
                pushScratchpad(noteOffEntry, noteEndTick);
              if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
                // The tee is DERIVED from the note-off entry above, so the two cannot disagree about
                // when the release happens. That rule used to live in a comment and had already been
                // broken once; see samplerNoteOffFor in apps/engine_rt_helpers.h.
                runtime.samplerEvents.push_back(daw::engine::samplerNoteOffFor(
                    noteOffEntry, blockSampleStart, engineConfig.blockSize, noteId));
              }
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              daw::engine::registerActiveNote(runtime, noteId, pitch, noteColumn,
                                              onTick, noteEndTick, noteTuningCents, true);
            }
          };

          // Drain row-op strikes (delay/retrigger) whose onset has reached this
          // window. Snapshot the due ones under the lock, then emit outside it so
          // emitNoteOnWithOff can re-take activeNotesMutex without deadlock.
          {
            std::vector<PendingStrike> due;
            {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              auto& pend = runtime.pendingStrikes;
              for (size_t i = 0; i < pend.size();) {
                if (pend[i].onTick >= rangeStart && pend[i].onTick < rangeEnd) {
                  due.push_back(pend[i]);
                  pend[i] = pend.back();
                  pend.pop_back();
                } else {
                  ++i;
                }
              }
            }
            for (const auto& s : due) {
              emitNoteOnWithOff(s.onTick, s.durationNanoticks, s.pitch,
                                s.velocity, s.column, s.tuningCents, s.sound, s.soundOffset);
            }
          }

          // First, check for any active notes that should end in this block
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            std::vector<uint32_t> notesToRemove;

            for (auto& [noteId, activeNote] : runtime.activeNotes) {
              if (!activeNote.hasScheduledEnd) {
                continue;
              }
              uint64_t offTick = activeNote.endNanotick;

              offTick = wrapTick(offTick);

              // Check if this note should end in the current block range
              if (offTick >= rangeStart && offTick < rangeEnd) {
                const uint64_t offDelta = baseTickDelta + (offTick - rangeStart);
                const uint64_t offSample = blockSampleStart + tickDeltaToSamples(offDelta);
                const int64_t offOffset = static_cast<int64_t>(offSample) - static_cast<int64_t>(blockSampleStart);

                if (offOffset >= 0 && offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                  daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                      offSample, 0, activeNote.pitch,
                      midiChannel, activeNote.tuningCents, activeNote.noteId);
                  pushScratchpad(noteOffEntry, activeNote.endNanotick);
              if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
                // The tee is DERIVED from the note-off entry above, so the two cannot disagree about
                // when the release happens. That rule used to live in a comment and had already been
                // broken once; see samplerNoteOffFor in apps/engine_rt_helpers.h.
                runtime.samplerEvents.push_back(daw::engine::samplerNoteOffFor(
                    noteOffEntry, blockSampleStart, engineConfig.blockSize, activeNote.noteId));
              }
                  notesToRemove.push_back(noteId);
                }
              }
            }

            // Remove notes that have ended
            for (uint32_t noteId : notesToRemove) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt != runtime.activeNotes.end()) {
                removeNoteIdFromColumn(noteIt->second.column, noteId);
              }
              runtime.activeNotes.erase(noteId);
            }
          }

          // Now process new notes starting in this range.
          //
          // THE SNAPSHOT IS HELD FOR THE WHOLE LOOP, not just for the range query. `events` is a
          // vector of RAW POINTERS into it, so the owning shared_ptr has to outlive their last
          // use — it used to be scoped to the `if`, leaving the only other owner as
          // runtime.clipSnapshot, which another thread replaces whenever the notes change. A
          // rebuild landing mid-window would then free the events being dispatched. Same family
          // as the use-after-free in #97, and PRE needs the snapshot down here anyway.
          std::vector<const daw::MusicalEvent*> events;
          auto snapshot = std::atomic_load_explicit(&runtime.clipSnapshot,
                                                    std::memory_order_acquire);
          if (snapshot) {
            getClipEventsInRange(*snapshot, rangeStart, rangeEnd, events);
          }
          for (const auto* event : events) {
            if (event->type == daw::MusicalEventType::Param) {
              const uint64_t tickDelta =
                  baseTickDelta + (event->nanotickOffset - rangeStart);
              const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                           samplesPerTick, engineConfig.blockSize);
              if (!placed) {
                continue;
              }
              const uint64_t eventSample = placed->sampleTime;
              daw::EventEntry paramEntry;
              paramEntry.sampleTime = eventSample;
              paramEntry.blockId = 0;
              paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
              paramEntry.size = sizeof(daw::ParamPayload);
              daw::ParamPayload payload{};
              std::memcpy(payload.uid16,
                          event->payload.param.uid16.data(),
                          sizeof(payload.uid16));
              payload.value = event->payload.param.value;
              payload.targetPluginIndex = paramTargetIndex;
              std::memcpy(paramEntry.payload, &payload, sizeof(payload));
              {
                std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
                runtime.paramMirror[event->payload.param.uid16] =
                    ParamMirrorEntry{payload.value, payload.targetPluginIndex};
              }
              pushScratchpad(paramEntry, event->nanotickOffset);
              continue;
            }
            if (event->type != daw::MusicalEventType::Note) {
              if (event->type != daw::MusicalEventType::Chord) {
                continue;
              }
              const uint64_t spread = event->payload.chord.spreadNanoticks;
              const uint64_t duration = event->payload.chord.durationNanoticks;
              const uint16_t humanizeTiming = event->payload.chord.humanizeTiming;
              const uint16_t humanizeVelocity = event->payload.chord.humanizeVelocity;
              const uint8_t baseVelocity = 100;
              const uint8_t column = event->payload.chord.column;

              const uint64_t chordDelta =
                  baseTickDelta + (event->nanotickOffset - rangeStart);
              const uint64_t chordSample =
                  blockSampleStart + tickDeltaToSamples(chordDelta);
              cutActiveNoteInColumn(column, chordSample, currentBlockId);

              const auto harmony = getHarmonyAt(event->nanotickOffset);
              if (!harmony.has_value()) {
                continue;
              }
              const auto* scale = getScaleForHarmony(*harmony);
              if (!scale) {
                continue;
              }
              const uint8_t rootPc = static_cast<uint8_t>(harmony->root % 12);
              auto chordPitches = daw::resolveChordPitches(
                  event->payload.chord.degree,
                  event->payload.chord.quality,
                  event->payload.chord.inversion,
                  event->payload.chord.baseOctave,
                  rootPc,
                  *scale);

              // THE CHORD PATH HAD NO TELEMETRY AT ALL, and nothing in this repo exercises it:
              // no fixture contains a chord and no check sends `do chord`. So "a chord resolved
              // and was scheduled" and "a chord was silently dropped for want of a scale" were
              // the same observation — nothing. This is the line that separates them, and it is
              // what made the strum measurable.
              //
              // ONCE PER CHORD, not once per pitch: a per-pitch event on a dense arrangement is
              // a log nobody reads, and the pitch count is the useful number anyway.
              DAW_EVENT("chord.scheduled")
                  .field("track", runtime.trackId)
                  .field("tick", event->nanotickOffset)
                  .field("pitches", static_cast<uint64_t>(chordPitches.size()))
                  .field("spread", spread)
                  .field("humanize_timing", static_cast<uint64_t>(humanizeTiming))
                  .field("humanize_velocity", static_cast<uint64_t>(humanizeVelocity));
              std::vector<PendingStrike> chordQueued;
              for (size_t i = 0; i < chordPitches.size(); ++i) {
                uint64_t offsetTicks = 0;
                if (chordPitches.size() > 1 && spread > 0) {
                  offsetTicks =
                      (spread * static_cast<uint64_t>(i)) /
                      static_cast<uint64_t>(chordPitches.size() - 1);
                }
                int jitter = daw::deterministicJitter(
                    event->payload.chord.chordId + static_cast<uint32_t>(i),
                    static_cast<int>(humanizeTiming));
                int64_t onTick = static_cast<int64_t>(event->nanotickOffset) +
                    static_cast<int64_t>(offsetTicks) + jitter;
                if (onTick < 0) {
                  onTick = 0;
                }
                int velJitter = daw::deterministicJitter(
                    event->payload.chord.chordId + static_cast<uint32_t>(i * 13),
                    static_cast<int>(humanizeVelocity));
                // EMITTED THROUGH emitNoteOnWithOff, NOT BY A SECOND COPY OF IT.
                //
                // This was ninety lines duplicating that lambda, and the duplicate was missing
                // one thing: the TEE TO THE BUILT-IN SAMPLER on the note-ON. It teed the
                // note-OFF and not the note-on — so every chord released a voice that had never
                // been started, and a chord played through the in-engine sampler was SILENT
                // while the same chord through a hosted plugin sounded correct. Measured: a note
                // and a chord in one fixture, same sampler, same render, note peak 9263 and
                // chord peak 0.
                //
                // The comment forty lines down describes the identical defect found earlier in
                // this same block, for the note-off's sample time. Two copies of one rule, twice,
                // in one function — which is the argument for there being one copy.
                //
                // Everything the duplicate did, the lambda does and does better: it handles the
                // block-boundary drop, both tees, the activeNotes bookkeeping, and the loop-point
                // wrap that the copy did not have.
                // EMIT NOW OR QUEUE FOR THE BLOCK THAT OWNS IT — the same shape the retrigger
                // path uses forty lines down, and for the same reason.
                //
                // A strike whose tick falls outside the range being filled cannot be emitted
                // here: emitNoteOnWithOff drops anything landing outside the block, silently.
                // The spread pushes every note after the first to a LATER tick, so a strum wider
                // than one block (11 ms at 512/44100) lost all but its first note — measured
                // with DAW_TRACE_NOTES as one note.emit per chord for a half-beat spread that
                // should produce three. The old hand-rolled emission did the same, so this was
                // never a strum, it was a chord with two notes deleted.
                const uint64_t strikeTick = wrapTick(static_cast<uint64_t>(onTick));
                const uint8_t strikePitch = clampMidi(chordPitches[i].midi);
                const uint8_t strikeVel =
                    clampMidi(static_cast<int>(baseVelocity) + velJitter);
                if (strikeTick >= rangeStart && strikeTick < rangeEnd) {
                  emitNoteOnWithOff(strikeTick, duration, strikePitch, strikeVel,
                                    column, chordPitches[i].cents);
                } else {
                  chordQueued.push_back(PendingStrike{strikeTick, duration, strikePitch,
                                                      strikeVel, column,
                                                      chordPitches[i].cents, 0, 0});
                }
              }
              daw::engine::queuePendingStrikes(runtime, chordQueued);
              continue;
            }
            const uint64_t tickDelta =
                baseTickDelta + (event->nanotickOffset - rangeStart);
            const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                         samplesPerTick, engineConfig.blockSize);
            if (!placed) {
              continue;
            }

            const uint8_t column = event->payload.note.column;
            // Length is stored, so playback infers nothing: no OFF sentinels
            // to interpret and no cut-on-next. A note sounds for exactly the
            // duration it carries, which is what the editor shows.
            if (event->payload.note.durationNanoticks == 0) {
              continue;
            }

            // Probability row op: a deterministic per-note roll (see helper).
            if (!daw::noteProbabilityPasses(
                    event->payload.note.noteId, event->nanotickOffset,
                    event->payload.note.pitch, column,
                    event->payload.note.probability)) {
              continue;
            }

            // CONDITIONAL TRIG. Different in kind from probability, which is why it is a separate
            // gate rather than another argument to that one: `pN` is a per-pass roll and
            // deliberately unpredictable, `1:2` is deterministic in WHICH PASS the transport is
            // on. That is what lets a phrase resolve every four bars instead of merely thinning.
            //
            // The pass index comes from transportElapsedNanotick — the transport's own unwrapped
            // position — and NEVER from a counter incremented here. A dispatch-side counter would
            // depend on how many blocks had run and how the note fell across them, and two
            // bounces of one project would differ.
            if (event->payload.note.trigCondition != daw::kTrigConditionNone) {
              // THE PASS OF THE NOTE, NOT OF THE BLOCK. `elapsed` is the transport's position at
              // the START of this block's window, and the dispatch looks AHEAD — so a note at
              // the top of pass N is emitted while the block is still in pass N-1. Reading the
              // block's pass made c1:2 sound on passes 0, 1 and 3 instead of 0 and 2: the gate
              // was firing correctly on the wrong number.
              //
              // `baseTickDelta` is how far into the window this SEGMENT begins — non-zero
              // exactly when the window straddled the loop end and emitNotes was called a second
              // time for the wrapped part, which is the next pass. Adding it and the note's own
              // offset within the segment gives the absolute tick the note actually sounds at,
              // which is the only position whose pass is the one the musician means.
              const uint64_t elapsed =
                  transportElapsedNanotick.load(std::memory_order_acquire);
              const uint64_t absoluteTick =
                  elapsed + baseTickDelta +
                  (event->nanotickOffset >= rangeStart ? event->nanotickOffset - rangeStart : 0);
              const uint64_t passIndex = loopLen > 0 ? (absoluteTick / loopLen) : 0;
              // PRE (#107) asks about a DIFFERENT note, so it is resolved against the track's
              // conditional list rather than by the per-note function. Finding this note's place
              // in that list is a binary search on a vector that holds only conditional trigs —
              // typically a handful — and it happens only for notes that carry a condition.
              //
              // Looked up by (TICK, COLUMN), which is unique — one note per column per row. It
              // used to match on (tick, code), and two PRE notes on one row therefore both found
              // the FIRST entry and resolved against the same predecessor. A chord of two
              // conditional notes is one keypress here, so that was not a corner case.
              bool fires = true;
              if (daw::isPreTrigCondition(event->payload.note.trigCondition)) {
                const auto& sites = snapshot->conditionals;
                size_t idx = sites.size();
                auto it = std::lower_bound(
                    sites.begin(), sites.end(), event->nanotickOffset,
                    [](const daw::TrigConditionSite& s, uint64_t t) { return s.tick < t; });
                for (; it != sites.end() && it->tick == event->nanotickOffset; ++it) {
                  if (it->column == event->payload.note.column) {
                    idx = static_cast<size_t>(it - sites.begin());
                    break;
                  }
                }
                fires = daw::conditionalTrigFires(sites.data(), sites.size(), idx, passIndex);
              } else {
                fires = daw::trigConditionFires(event->payload.note.trigCondition, passIndex);
              }
              if (!fires) {
                continue;
              }
            }

            daw::ResolvedPitch resolved =
                daw::resolvedPitchFromCents(static_cast<double>(event->payload.note.pitch) * 100.0);
            if (auto harmony = getHarmonyAt(event->nanotickOffset)) {
              if (trackState.harmonyQuantize) {
                resolved = quantizePitch(event->payload.note.pitch, *harmony);
              }
            }
            const uint8_t scheduledPitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint64_t noteDuration = event->payload.note.durationNanoticks;
            const uint8_t velocity = event->payload.note.velocity;

            // Time-spreading row ops (delay, retrigger): expand the note into its
            // strikes and route each through the shared emitter — inline if it
            // lands in this window, else queued for the block that reaches it.
            // The op-free path is one strike at the note's own tick, which is
            // always in-window here (its start is why we are in this block), so
            // it takes the fast inline branch below.
            const uint8_t retrig = event->payload.note.retrigger;
            const uint32_t delayTicks = event->payload.note.delayNanoticks;
            if (retrig > 1 || delayTicks > 0) {
              const auto strikes = daw::expandNoteOps(
                  event->nanotickOffset, noteDuration, retrig, delayTicks,
                  event->payload.note.retrigRamp);
              std::vector<PendingStrike> queued;
              // THE RAMP IS APPLIED HERE, ONCE, so a queued strike carries the velocity it will
              // sound at rather than a scale somebody downstream has to remember to apply. A
              // PendingStrike that stored the authored velocity plus a factor would be two facts
              // about one thing, and the queue path is exactly where the second one gets lost —
              // which is how a retriggered note's later strikes lost their sound address before.
              // The floor-of-1 rule moved to apps/engine_rt_helpers.h, where a test pins it:
              // a ramp reaching velocity 0 emits a note-OFF and hangs the voice.
              auto rampedVelocity = [&](uint16_t scaleMilli) -> uint8_t {
                return daw::engine::rampedVelocity(velocity, scaleMilli);
              };
              for (const auto& s : strikes) {
                const uint64_t onTick = wrapTick(s.onTick);
                const uint64_t dur =
                    s.offTick > s.onTick ? s.offTick - s.onTick : 0;
                const uint8_t strikeVelocity = rampedVelocity(s.velocityScaleMilli);
                if (onTick >= rangeStart && onTick < rangeEnd) {
                  emitNoteOnWithOff(onTick, dur, scheduledPitch, strikeVelocity, column,
                                    tuningCents, event->payload.note.sound,
                                    event->payload.note.soundOffset);
                } else {
                  queued.push_back(PendingStrike{onTick, dur, scheduledPitch,
                                                 strikeVelocity, column, tuningCents,
                                                 event->payload.note.sound,
                                                 event->payload.note.soundOffset});
                }
              }
              daw::engine::queuePendingStrikes(runtime, queued);
            } else {
              emitNoteOnWithOff(event->nanotickOffset, noteDuration,
                                scheduledPitch, velocity, column, tuningCents,
                                event->payload.note.sound,
                                event->payload.note.soundOffset);
            }
          }
        };
        auto flagRingOverflow = [&](uint64_t sampleTime,
                                    uint32_t droppedCount,
                                    bool midiDropped) {
          if (droppedCount > 0) {
            runtime.ringStdDropCount.fetch_add(droppedCount, std::memory_order_relaxed);
          }
          runtime.ringStdDropSample.store(sampleTime, std::memory_order_relaxed);
          runtime.ringStdOverflowed.store(true, std::memory_order_relaxed);
          if (midiDropped) {
            runtime.ringStdPanicPending.store(true, std::memory_order_release);
          }
          if (!runtime.mirrorPending.load(std::memory_order_acquire)) {
            enqueueMirrorReplay(runtime);
          }
        };
        auto flushPendingNoteOffs = [&](uint64_t sampleTime,
                                        uint32_t currentBlockId) {
          if (!runtime.ringStdPanicPending.load(std::memory_order_acquire)) {
            return;
          }
          std::vector<ActiveNote> pendingNotes;
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            pendingNotes.reserve(runtime.activeNotes.size());
            for (const auto& [noteId, activeNote] : runtime.activeNotes) {
              pendingNotes.push_back(activeNote);
            }
          }
          if (pendingNotes.empty()) {
            runtime.ringStdPanicPending.store(false, std::memory_order_release);
            return;
          }
          std::vector<uint32_t> clearedNotes;
          clearedNotes.reserve(pendingNotes.size());
          for (const auto& activeNote : pendingNotes) {
            daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                sampleTime, currentBlockId, activeNote.pitch,
                midiChannel, activeNote.tuningCents, activeNote.noteId);
            if (!daw::ringWrite(ringStd, noteOffEntry)) {
              runtime.ringStdOverflowed.store(true, std::memory_order_relaxed);
              return;
            }
            clearedNotes.push_back(activeNote.noteId);
          }
          if (!clearedNotes.empty()) {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            for (uint32_t noteId : clearedNotes) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt == runtime.activeNotes.end()) {
                continue;
              }
              const uint8_t column = noteIt->second.column;
              runtime.activeNotes.erase(noteIt);
              removeNoteIdFromColumn(column, noteId);
            }
          }
          runtime.ringStdPanicPending.store(false, std::memory_order_release);
        };

        for (const auto& automationClip : trackState.automationClips) {
          const auto uid16 = daw::hashStableId16(automationClip.paramId());
          if (automationClip.discreteOnly()) {
            const auto split = daw::engine::splitWindowAtLoopEnd(
                windowStartTicks, windowEndTicks, loopStartTicks, loopEndTicks);
            for (uint32_t si = 0; si < split.count; ++si) {
              const auto& seg = split.segments[si];
              emitAutomationPoints(automationClip, seg.startTick, seg.endTick,
                                   seg.baseTickDelta, uid16);
            }
          } else {
            float lastValue = 0.0f;
            bool hasLast = false;
            uint32_t targetIndex = automationClip.targetPluginIndex();
            if (targetIndex == daw::kParamTargetAll) {
              targetIndex = paramTargetIndex;
            }
            {
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              const auto it = runtime.paramMirror.find(uid16);
              if (it != runtime.paramMirror.end()) {
                lastValue = it->second.value;
                if (it->second.targetPluginIndex != daw::kParamTargetAll) {
                  targetIndex = it->second.targetPluginIndex;
                }
                hasLast = true;
              }
            }
            constexpr float kAutomationEpsilon = 1.0e-5f;
            for (uint32_t offset = 0; offset < engineConfig.blockSize; ++offset) {
              const uint64_t tickDelta =
                  static_cast<uint64_t>(std::llround(
                      static_cast<long double>(offset) *
                      static_cast<long double>(blockTicks) /
                      static_cast<long double>(engineConfig.blockSize)));
              uint64_t tick = windowStartTicks + tickDelta;
              tick = wrapTick(tick);
              const float value = automationClip.valueAt(tick);
              if (hasLast && std::fabs(value - lastValue) <= kAutomationEpsilon) {
                continue;
              }
              daw::EventEntry paramEntry;
              paramEntry.sampleTime = blockSampleStart + offset;
              paramEntry.blockId = 0;
              paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
              paramEntry.size = sizeof(daw::ParamPayload);
              daw::ParamPayload payload{};
              std::memcpy(payload.uid16, uid16.data(), uid16.size());
              payload.value = value;
              payload.targetPluginIndex = targetIndex;
              std::memcpy(paramEntry.payload, &payload, sizeof(payload));
              {
                std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
                runtime.paramMirror[uid16] = ParamMirrorEntry{value, targetIndex};
              }
              pushScratchpad(paramEntry, tick);
              lastValue = value;
              hasLast = true;
            }
          }
        }

        {
          const auto split = daw::engine::splitWindowAtLoopEnd(
              windowStartTicks, windowEndTicks, loopStartTicks, loopEndTicks);
          for (uint32_t si = 0; si < split.count; ++si) {
            const auto& seg = split.segments[si];
            emitNotes(seg.startTick, seg.endTick, seg.baseTickDelta);
          }
        }

        auto applyModUpdates = [&]() {
          if (modUpdates.empty()) {
            return;
          }
          std::lock_guard<std::mutex> lock(runtime.modSourcesMutex);
          auto& sources = runtime.modSources;
          for (const auto& update : modUpdates) {
            bool updated = false;
            for (auto& source : sources) {
              if (source.ref.deviceId == update.ref.deviceId &&
                  source.ref.sourceId == update.ref.sourceId &&
                  source.ref.kind == update.ref.kind) {
                source.value = update.value;
                updated = true;
                break;
              }
            }
            if (!updated) {
              sources.push_back(update);
            }
          }
        };

        applyModUpdates();

        // The three rules inside — source strictly upstream of target, VstParam targets only, and
        // clamp01(bias + depth * source) — moved to apps/engine_rt_helpers.h with tests. None of
        // them had one before.
        const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>
            resolveDevicePluginPathFnRt = resolveDevicePluginPath;
        daw::engine::BlockModCtx blockModCtx{runtime, trackState, blockSampleStart,
                                             windowStartTicks, noteCutCtx,
                                             resolveDevicePluginPathFnRt};
        auto applyBlockRateModulation = [&]() {
          daw::engine::applyBlockRateModulation(blockModCtx);
        };

        applyBlockRateModulation();

        const bool eventDirty = scratchpadCount > 0;
        bool resolvedEvents = false;
        auto resolveAndSort = [&]() {
          if (resolvedEvents) {
            return;
          }
          uint32_t outCount = 0;
          auto appendScratchpad = [&](const daw::EventEntry& entry,
                                      uint64_t overflowTick) -> bool {
            if (outCount < scratchpad.size()) {
              scratchpad[outCount++] = entry;
              return true;
            }
            daw::atomic_store_u64(
                reinterpret_cast<uint64_t*>(&lastOverflowTick), overflowTick);
            return false;
          };
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            auto entry = scratchpad[i];
            if (static_cast<daw::EventType>(entry.type) != daw::EventType::MusicalLogic) {
              scratchpad[outCount++] = entry;
              continue;
            }
            daw::MusicalLogicPayload logic{};
            std::memcpy(&logic, entry.payload, sizeof(logic));
            if (logic.metadata[0] == daw::kMusicalLogicKindGate) {
              continue;
            }
            int64_t offsetSamples =
                static_cast<int64_t>(entry.sampleTime) -
                static_cast<int64_t>(blockSampleStart);
            // CLAMPED INTO THE BLOCK, NOT DROPPED.
            //
            // Everything in this scratchpad was generated FOR this block's TICK window, so it
            // belongs to this block by construction. Its sample time is a CONVERSION of that
            // tick, and a conversion can land a sample outside: at 120 bpm and 44.1 kHz the
            // sixteenth at 1.875 s sits at sample 82687.5, so a block covering [82432, 82688)
            // converts it to 82688 — the first sample of the NEXT block. This test dropped it
            // there, and the next block never emitted it either, because its TICK window starts
            // after that step. The note simply vanished.
            //
            // It bites only when a step lands almost exactly on a block boundary, so WHICH notes
            // vanish depends on the buffer size: at 256 frames that sixteenth is on a boundary
            // and is lost, at 1024 it is not and it plays. One missing note in an eight-second
            // render at one buffer size — which is why it survived until a check demanded that
            // two renders be BIT-IDENTICAL rather than merely similar.
            //
            // Half a sample early is inaudible; a missing note is not. Widening the window
            // instead would let the same event be emitted by two consecutive blocks, which is a
            // doubled note rather than a missing one — no better.
            if (offsetSamples < 0 ||
                offsetSamples >= static_cast<int64_t>(engineConfig.blockSize)) {
              // SAID OUT LOUD, ONCE. With the generator's floor conversion in place this cannot
              // fire — its own negative control passes, which is the honest way to describe a
              // guard that no longer has a reproducer. It is kept because the CLIP path converts
              // ticks to samples by a different route that has not been audited for the same
              // property, and because the alternative behaviour here was to DELETE the note.
              //
              // If it ever does fire, this line is the difference between a diagnosable report
              // and another year of "a note goes missing sometimes".
              if (!warnedEventOutsideBlock.exchange(true, std::memory_order_relaxed)) {
                DAW_EVENT("patcher.event_outside_block")
                    .field("offset", offsetSamples)
                    .field("block_size", static_cast<uint32_t>(engineConfig.blockSize));
              }
              offsetSamples = offsetSamples < 0
                                  ? 0
                                  : static_cast<int64_t>(engineConfig.blockSize) - 1;
            }
            const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                static_cast<long double>(offsetSamples) / samplesPerTick));
            uint64_t eventTick = windowStartTicks + tickDelta;
            eventTick = wrapTick(eventTick);
            const auto harmony = getHarmonyAt(eventTick);
            if (!harmony.has_value()) {
              continue;
            }
            const auto* scale = getScaleForHarmony(*harmony);
            if (!scale) {
              continue;
            }
            const uint8_t rootPc = static_cast<uint8_t>(harmony->root % 12);
            const uint8_t baseOctave = daw::engine::resolvedBaseOctave(
                logic.base_octave, static_cast<int32_t>(logic.octave_offset));
            const daw::ResolvedPitch resolved =
                daw::resolveDegree(logic.degree, baseOctave, rootPc, *scale);
            const uint8_t velocity = daw::engine::resolvedVelocity(logic.velocity);
            const uint8_t pitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint8_t channel = midiChannel;
            const uint32_t noteId =
                nextNoteId.fetch_add(1, std::memory_order_acq_rel);

            // Rewrites the MusicalLogic entry in place: its sampleTime and blockId are already
            // correct and must survive, so they are fed back in rather than re-derived.
            entry = daw::engine::makeNoteOnEntry(entry.sampleTime, entry.blockId, pitch, velocity,
                                                 channel, tuningCents, noteId,
                                                 kEventFlagMusicalLogic);
            scratchpad[outCount++] = entry;
            // TEE TO THE BUILT-IN SAMPLER. Without this a patcher could not play the sampler at
            // all: every one of the six existing tees is on the CLIP path, so a Euclidean or
            // RandomDegree node produced MIDI that reached a hosted plugin and an in-engine
            // instrument on the same track never heard a note. Verified by rendering exactly
            // that project and getting a peak of zero.
            //
            // It is the same tee the clip path does, and it has to be: `sound` is 0 here because
            // the patcher's MusicalLogicPayload carries no sound address, so the KEYMAP picks the
            // slot from the resolved pitch — which is the right default and the one every drum
            // kit already relies on. A node that chooses a slice (docs/SAMPLER_DESIGN.md's
            // SliceSelect) is what would fill that field, and it needs this path to exist first.
            if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
              // THE SOUND ADDRESS THE GRAPH CHOSE, if it chose one. This was hardcoded 0 —
              // "no address, let the keymap pick from the pitch" — because nothing upstream
              // could supply one. SliceSelect now can, which is the whole point of the node:
              // a generated note that names its slice rather than inheriting whatever the
              // resolved pitch happens to map to. Still 0 for every other graph, which is
              // still the right default and what every drum kit relies on.
              //
              // A GENERATED note obeys the track's rule too: a graph that names no slice on a
              // sound-addressed-only track must not silently fall back to pitch selection,
              // which is the behaviour the track was explicitly switched out of.
              runtime.samplerEvents.push_back(daw::engine::samplerNoteOnFor(
                  static_cast<uint32_t>(offsetSamples), pitch, velocity, /*column=*/0,
                  logic.sound, /*offsetFrac=*/0, trackState.soundAddressedOnly, noteId));
            }

            if (logic.duration_ticks > 0) {
              const uint64_t noteEndTick = eventTick + logic.duration_ticks;
              uint64_t offTick = wrapTick(noteEndTick);
              if (offTick >= windowStartTicks && offTick < windowEndTicks) {
                const uint64_t offDelta = offTick - windowStartTicks;
                const uint64_t offSample =
                    blockSampleStart + tickDeltaToSamples(offDelta);
                const int64_t offOffset =
                    static_cast<int64_t>(offSample) -
                    static_cast<int64_t>(blockSampleStart);
                if (offOffset >= 0 &&
                    offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                  daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                      offSample, 0, pitch,
                      channel, tuningCents, noteId,
                      kEventFlagMusicalLogic);
                  appendScratchpad(noteOffEntry, noteEndTick);
                  if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
                    daw::SamplerEvent se;
                    se.offsetInBlock = static_cast<uint32_t>(offOffset);
                    se.kind = daw::SamplerEventKind::NoteOff;
                    se.pitch = pitch;
                    se.velocity = 0;
                    se.column = 0;
                    se.noteId = noteId;
                    runtime.samplerEvents.push_back(se);
                  }
                }
              } else {
                std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                daw::engine::registerActiveNote(runtime, noteId, pitch, 0,
                                                eventTick, noteEndTick, tuningCents, true);
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              daw::engine::registerActiveNote(runtime, noteId, pitch, 0,
                                              eventTick, eventTick, tuningCents, false);
            }
          }
          scratchpadCount = outCount;
          std::stable_sort(scratchpad.begin(), scratchpad.begin() + scratchpadCount,
                           [&](const daw::EventEntry& a, const daw::EventEntry& b) {
                             const auto pa = priorityForEvent(a);
                             const auto pb = priorityForEvent(b);
                             return std::tie(a.sampleTime, pa) <
                                 std::tie(b.sampleTime, pb);
                           });
          resolvedEvents = true;
        };

        if (eventDirty) {
          resolveAndSort();
        }

        if (routedMidi) {
          routedMidi->clear();
          routedMidi->reserve(scratchpadCount);
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            const auto& entry = scratchpad[i];
            if (entry.type == static_cast<uint16_t>(daw::EventType::Midi)) {
              routedMidi->push_back(entry);
            }
          }
        }

        const uint64_t panicSampleTime =
            latencyMgr.getCompensatedStart(blockSampleStart);
        flushPendingNoteOffs(panicSampleTime, currentBlockId);
        if (scratchpadCount > 0) {
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            auto entry = scratchpad[i];
            entry.blockId = currentBlockId;
            entry.sampleTime = latencyMgr.getCompensatedStart(entry.sampleTime);
            if (entry.type == static_cast<uint16_t>(daw::EventType::Param) &&
                entry.size >= sizeof(daw::ParamPayload) &&
                paramTargetIndex != daw::kParamTargetAll) {
              daw::ParamPayload payload{};
              std::memcpy(&payload, entry.payload, sizeof(payload));
              if (payload.targetPluginIndex == daw::kParamTargetAll) {
                payload.targetPluginIndex = paramTargetIndex;
                std::memcpy(entry.payload, &payload, sizeof(payload));
              }
            }
            if (!daw::ringWrite(ringStd, entry)) {
              bool midiDropped = false;
              for (uint32_t j = i; j < scratchpadCount; ++j) {
                const auto& dropped = scratchpad[j];
                if (dropped.type != static_cast<uint16_t>(daw::EventType::Midi)) {
                  continue;
                }
                if (dropped.size < sizeof(daw::MidiPayload)) {
                  continue;
                }
                daw::MidiPayload payload{};
                std::memcpy(&payload, dropped.payload, sizeof(payload));
                const uint8_t type = payload.status & 0xF0u;
                if (type == 0x80u || type == 0x90u) {
                  midiDropped = true;
                  break;
                }
              }
              flagRingOverflow(entry.sampleTime,
                               scratchpadCount - i,
                               midiDropped);
              break;
            }
          }
        }
        return patcherAudioWritten.load(std::memory_order_relaxed);
  }
}

}  // namespace daw::engine
