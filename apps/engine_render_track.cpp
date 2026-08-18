// Bodies for apps/engine_render_track.h. The body below is the lambda that lived in
// main(), moved WITHOUT EDITS: the references bound at the top carry the names it
// captured, so nested lambdas, shadowing and the brace scope all still mean what they
// meant. Rewriting 1,300 lines to say `deps.x` would have touched every one of them.
#include "apps/engine_render_track.h"

#include <filesystem>
#include <optional>
#include "apps/engine_producer_helpers.h"
#include "apps/engine_emit_notes.h"
#include "apps/engine_resolve_events.h"
#include "apps/engine_run_patcher_node.h"

#include "apps/engine_rt_helpers.h"
#include "apps/engine_sampler_commands.h"
#include "apps/event_log.h"
#include "apps/uid_hash.h"
#include "apps/chord_resolver.h"

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
  auto& harmonyEvents = deps.harmonyTimeline.harmonyEvents;
  auto& harmonyMutex = deps.harmonyTimeline.harmonyMutex;
  auto& lastOverflowTick = deps.lastOverflowTick;
  auto& patcherAssembledFromDevices = deps.engineState.patcherGraph.patcherAssembledFromDevices;
  auto& patcherGraphSnapshot = deps.engineState.patcherGraph.patcherGraphSnapshot;
  auto& patcherParallel = deps.patcherParallel;
  auto& patcherPool = deps.patcherPool;
  auto& tempoProvider = deps.tempoProvider;
  const auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
  const auto& wrapTick = deps.noteResolution.wrapTick;
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
          // AN EVENT STAMPED FOR A LATER BLOCK IS KEPT, NOT SWALLOWED.
          //
          // The drain above takes the WHOLE queue, so anything this loop declines is gone. A
          // routing SOURCE stamps its events for the NEXT block (nextBlockSampleStart, at the end
          // of its processTrack) and the destination drains against THIS one — so when the
          // destination is processed after the source in the same block, every routed event was
          // consumed here and discarded.
          //
          // That made track-to-track MIDI routing depend on TRACK-ID ORDER: routing 1 -> 0 played,
          // routing 0 -> 1 was silent, in the same project with the ids swapped. Measured with
          // tools/midi_route_check.sh, which renders both directions.
          std::vector<daw::EventEntry> deferred;
          for (const auto& entry : inboundEvents) {
            if (entry.sampleTime >= blockSampleEnd) {
              deferred.push_back(entry);
              continue;
            }
            if (entry.sampleTime < blockSampleStart) {
              // Genuinely in the past: the block that owned it has already been rendered, so
              // holding it would emit it at the wrong time rather than late.
              continue;
            }
            int64_t offsetSamples =
                static_cast<int64_t>(entry.sampleTime) -
                static_cast<int64_t>(blockSampleStart);
            // NO CLAMP HERE, AND NONE IS REACHABLE. The window test eight lines above bounds
            // entry.sampleTime to [blockSampleStart, blockSampleStart + blockSize), and nothing
            // between the two touches either value, so offsetSamples is in [0, blockSize) by
            // construction. This carried a 44-line copy of the clamp that engine_resolve_events
            // needs — including a DAW_EVENT described as "the difference between a diagnosable
            // report and another year of a note goes missing sometimes", which could never fire.
            //
            // Proven twice before deleting: an abort() here does not trigger while
            // tools/midi_route_check.sh renders both routing directions, and the SAME abort under
            // an inverted condition does. The first probe of this branch was worthless — the
            // enclosing loop had no fixture at all then, so it could not have fired whatever the
            // code did. The live copy of the rule is in apps/engine_resolve_events.cpp, where the
            // offset is NOT pre-bounded.
            const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                static_cast<long double>(offsetSamples) / samplesPerTick));
            const uint64_t eventTick = wrapTick(windowStartTicks + tickDelta);
            pushScratchpad(entry, eventTick);
          }
          // Put the future ones back for the block they belong to. Appended rather than assigned:
          // the source may already have enqueued more while this ran.
          if (!deferred.empty()) {
            std::lock_guard<std::mutex> lock(runtime.inboundMutex);
            runtime.inboundMidiEvents.insert(runtime.inboundMidiEvents.end(),
                                             deferred.begin(), deferred.end());
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
        // THE FILTER FAILS CLOSED WHEN THE POOL IS PER-DEVICE, and until this line it failed open.
        //
        // The loop above asks "does THIS track carry a patcher device". A track carrying none left
        // useNodeFilter false, and false does not mean "filter by something else" — it disables the
        // ownership guard in runNode entirely and takes the branch that runs EVERY node in the pool.
        // The pool is global: reassemblePatcherFromDevices concatenates every track's subgraph into
        // one graph with re-id'd nodes. So a track with no patcher ran every other track's nodes and
        // merged their events into its own scratchpad, and therefore its own instrument.
        //
        // Measured, two tracks, offline render: track 0 held the patcher AND a 300 Hz sampler,
        // track 1 held a 900 Hz sampler and no patcher, no clip, no placement and no note. The
        // render came out at 900 Hz on track 1's pan side, with track 0 SILENT — the generated
        // notes played the wrong track's instrument rather than merely also playing it.
        //
        // ASKING THE TRACK WAS NEVER THE RIGHT QUESTION; the right one is what kind of pool this is,
        // and the answer already existed on the engine as patcherAssembledFromDevices. It was not
        // reachable from here: RenderTrackDeps did not carry it, so the render path could not tell a
        // per-device pool from a legacy whole-project graph and guessed from the only thing it could
        // see. When the pool IS per-device every node belongs to exactly one device on exactly one
        // track, so every track must filter — and a track that owns nothing runs nothing.
        //
        // The legacy branch stays for a pool that is NOT per-device assembled, where the graph
        // genuinely belongs to the project rather than to a device and every track running it is
        // the defined behaviour.
        if (patcherAssembledFromDevices.load(std::memory_order_acquire)) {
          useNodeFilter = true;
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
        // WHICH DEVICES HOLD A HOST SLOT — the same test rebuildHostForChain applies when it
        // builds `pluginPaths`, INCLUDING its Direct-with-a-real-path case. A device that loads
        // by path holds a slot exactly like one that resolved through the scan, and a walk that
        // missed it would number every later device one too low.
        const auto occupiesSlot = [&](const daw::Device& device) {
          if (device.hostSlotIndex == daw::kHostSlotIndexDirect &&
              !device.vstRef.path.empty() &&
              std::filesystem::exists(device.vstRef.path)) {
            return true;
          }
          return static_cast<bool>(resolveDevicePluginPath(runtime, device.hostSlotIndex));
        };
        // THE ALL-TARGET FALLBACK IS A PREFERENCE OVER THAT WALK, not a different walk.
        //
        // It picks the first hosted device that is NOT bypassed, which is a sensible choice for a
        // parameter with no named target. Bypass belongs HERE and not in the slot rule: it decides
        // which plugin to prefer, never which index a plugin has. Conflating the two put the
        // preference into the address and aimed named targets at the wrong plugin.
        uint32_t paramTargetIndex = daw::kParamTargetAll;
        daw::forEachHostedDevice(trackState.chainDevices, occupiesSlot,
                                 [&](uint32_t index, const daw::Device& device) {
                                   if (device.bypass) {
                                     return true;  // keep looking; it still holds slot `index`
                                   }
                                   paramTargetIndex = index;
                                   return false;
                                 });
        // A STABLE DEVICE ID -> THE COMPACT HOST INDEX THAT NAMES IT RIGHT NOW.
        //
        // The compact index is a position in the host's own plugin list, counted over the
        // resolvable non-bypassed VST devices of this track — the same walk as above. It is a
        // LIVE value, which is exactly why it is no longer what a lane persists
        // (apps/automation_target.h); an automation target names a device, and this is where that
        // durable name becomes the thing the host can act on.
        //
        // TEMPORARY SHAPE, STATED AS SUCH: it derives the mapping from `trackState.chainDevices`,
        // which R-HOST-PLAN-AUTHORITY removes as an execution authority in a later step of this
        // same change. The ExecutionSnapshot's per-track plan carries the resolved compact index
        // and this walk goes with it.
        //
        // NOTHING, NOT kParamTargetAll, WHEN THE DEVICE IS NOT CURRENTLY HOSTED. An earlier
        // version returned the all-target sentinel here, and that is a SILENT WIDENING: a lane
        // aimed at one device whose plugin is bypassed or unresolvable would have broadcast every
        // one of its points to EVERY plugin on the track. The user asked for one device; the
        // correct answer when that device is not there is none, not all.
        const auto compactIndexForDevice = [&](uint32_t stableDeviceId) -> std::optional<uint32_t> {
          std::optional<uint32_t> found;
          daw::forEachHostedDevice(trackState.chainDevices, occupiesSlot,
                                   [&](uint32_t index, const daw::Device& device) {
                                     if (device.id != stableDeviceId) {
                                       return true;
                                     }
                                     found = index;
                                     return false;
                                   });
          return found;
        };
        // ONE PLACE A LANE'S TARGET BECOMES A NUMBER, so the two emit sites below cannot drift.
        // A disabled target returns nothing at all and the caller skips the lane.
        const auto resolveLaneTarget =
            [&](const daw::AutomationClip& lane) -> std::optional<uint32_t> {
          switch (lane.target().kind) {
            case daw::AutomationTargetKind::All:
              return paramTargetIndex;
            case daw::AutomationTargetKind::StableDevice:
              return compactIndexForDevice(lane.target().stableDeviceId);
            case daw::AutomationTargetKind::DisabledLegacyCompact:
              return std::nullopt;
          }
          return std::nullopt;
        };
        std::atomic<bool> patcherAudioWritten{false};
        // ONE NODE, RUN — apps/engine_run_patcher_node.h. This was a 193-line lambda over twelve
        // implicit captures; eight of them were already reachable through `deps` and `runtime`.
        auto runNode = [&](uint32_t nodeIndex) {
          runPatcherNode(deps.engineConfig, deps.lastOverflowTick, deps.projectSeed,
                         deps.tempoProvider, runtime, graphSnapshot, nodeIndex, nodeCount,
                         useNodeFilter,
                         blockSampleStart, windowStartTicks, windowEndTicks, harmonySnapshot,
                         harmonyCount, patcherAudioWritten);
        };

        // A NODE THIS TRACK DOES NOT RUN MUST NOT STILL HOLD LAST BLOCK'S EVENTS. runNode zeroes
        // buffer.count only for nodes it actually runs, and it returns BEFORE that zeroing when the
        // ownership guard rejects a node — while mergeNodeBuffers below walks the whole topoOrder
        // unconditionally. So a buffer left populated is re-emitted at its stale sampleTime on every
        // block from then on. That was survivable while every track ran every node (nothing was ever
        // skipped); with the filter now closed it would be the failure mode replacing the leak.
        for (uint32_t i = 0; i < nodeCount && i < nodeBuffers.size(); ++i) {
          nodeBuffers[i].count = 0;
        }
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
          // AND WHEN useNodeFilter IS SET WITH AN EMPTY chainOrder, neither branch runs: the pool is
          // per-device and this track owns no node in it. That is the case the old `else` swallowed
          // — it read "not filtered" as "run everything", which is precisely backwards for a track
          // that owns nothing.
        } else if (!useNodeFilter) {
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
          const auto resolved = resolveLaneTarget(automationClip);
          if (!resolved) {
            return;  // a disabled legacy target: the lane keeps its points and dispatches none
          }
          uint32_t targetIndex = *resolved;
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
        // THE NOTES IN A TICK RANGE — apps/engine_emit_notes.h. This was a 473-line lambda over
        // seventeen implicit captures, the largest single thing inside renderTrack. The wrapper
        // stays so the loop-split call site below reads unchanged.
        auto emitNotes = [&](uint64_t rangeStart, uint64_t rangeEnd, uint64_t baseTickDelta) {
          emitNotesInRange(deps.noteResolution, deps.engineConfig, deps.traceNotes,
                           deps.engineState.transport, runtime, trackState, noteCutCtx, rangeStart, rangeEnd,
                           baseTickDelta, blockSampleStart, loopEndTicks, loopLen,
                           samplesPerTick, midiChannel, currentBlockId, paramTargetIndex);
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
          // UNCONDITIONAL. The `if (!mirrorPending)` this replaces dropped an overflow that arrived
          // while a relaunch replay was in flight: that replay would complete, and the parameters the
          // ring dropped were never re-sent. Arming is re-entrant, so both causes are recorded and one
          // replay serves both.
          enqueueMirrorReplay(runtime, daw::kMirrorCauseOverflow);
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
            const auto resolvedContinuous = resolveLaneTarget(automationClip);
            if (!resolvedContinuous) {
              continue;  // a disabled legacy target dispatches nothing; its points are kept
            }
            float lastValue = 0.0f;
            bool hasLast = false;
            uint32_t targetIndex = *resolvedContinuous;
            {
              // THE MIRROR SUPPLIES THE LAST VALUE, NOT THE TARGET — and it used to supply both.
              //
              // The old override read `if (mirror.targetPluginIndex != kParamTargetAll)
              // targetIndex = mirror.targetPluginIndex;`. That was harmless while a lane's target
              // was a static persisted number, because the two agreed. The target is RE-DERIVED
              // every block now (a device's compact slot moves when the chain changes), so the
              // override became a LATCH: the first concrete index the mirror ever saw won forever,
              // and bypassing or adding a device left the lane driving the old slot with nothing
              // to say so.
              //
              // The lane's own resolved target is authoritative. The mirror is a de-duplication
              // cache for the VALUE — `hasLast` below skips emitting a point that would not change
              // anything — and that is all it is used for here.
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              const auto it = runtime.paramMirror.find(uid16);
              if (it != runtime.paramMirror.end()) {
                lastValue = it->second.value;
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
        // MUSICAL LOGIC RESOLVES TO NOTES, AND THE BLOCK SORTS — apps/engine_resolve_events.h.
        // This was a 179-line lambda over eighteen implicit captures. Seven of them were already
        // RenderTrackDeps members and travel as `deps`; one was a memo for a second call that
        // could not happen and did not travel at all.
        if (eventDirty) {
          scratchpadCount = resolveMusicalLogicAndSort(
              deps.noteResolution, deps.engineConfig, deps.lastOverflowTick,
              deps.warnedEventOutsideBlock, runtime, trackState, scratchpad, scratchpadCount,
              blockSampleStart,
              windowStartTicks, windowEndTicks, samplesPerTick, midiChannel);
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

        // STAMPED IN ENGINE SAMPLES, which is the clock the host windows this block against. The
        // pipeline-depth offset that used to be subtracted here cancelled against the block start
        // the host was given, and below latencySamples_ it could not be subtracted at all: every
        // event in the first (numBlocks-1) blocks came out at the same instant, summed. See
        // apps/latency_manager.h for the measurement.
        const uint64_t panicSampleTime = blockSampleStart;
        flushPendingNoteOffs(panicSampleTime, currentBlockId);
        if (scratchpadCount > 0) {
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            auto entry = scratchpad[i];
            entry.blockId = currentBlockId;
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
