#include "apps/engine_run_patcher_node.h"

// THE BODY BELOW IS VERBATIM — renderTrack's `runNode` lambda, unedited. Every name it captured is
// either a parameter with the same name or bound to one in the preamble, so the move is provable
// by diffing this range against the lambda in the parent commit.
#include <cstring>

#include "apps/engine_producer_helpers.h"
#include "apps/engine_pure.h"
#include "apps/engine_rt_helpers.h"
#include "apps/event_log.h"

namespace daw::engine {

void runPatcherNode(const daw::HostConfig& engineConfig,
                    std::atomic<uint64_t>& lastOverflowTick,
                    std::atomic<uint64_t>& projectSeed,
                    daw::TempoMapProvider& tempoProvider,
                    TrackRuntime& runtime,
                    const daw::PatcherGraph& graphSnapshot,
                    uint32_t nodeIndex,
                    uint32_t nodeCount,
                    bool useNodeFilter,
                    uint64_t blockSampleStart,
                    uint64_t windowStartTicks,
                    uint64_t windowEndTicks,
                    const std::array<daw::HarmonyEvent, daw::kUiMaxHarmonyEvents>& harmonySnapshot,
                    uint32_t harmonyCount,
                    std::atomic<bool>& patcherAudioWritten) {
  // FOUR ARGUMENTS INSTEAD OF AN EIGHTEEN-MEMBER STRUCT. It used four of the eighteen and they
  // are not a group — a node needs the block's config, the seed it hashes against, the tempo and
  // somewhere to report an overflow — so they are named individually. The four TrackRuntime
  // members below are still bound, so the body reads exactly as it did.
  auto& nodeBuffers = runtime.patcherNodeBuffers;
  auto& nodeAllowed = runtime.patcherNodeAllowed;
  auto& nodeModOutputs = runtime.patcherNodeModOutputs;
  auto& modUpdates = runtime.patcherModUpdates;
  auto& modInputSamples = runtime.patcherModInputSamples;
  auto& modOutputSamples = runtime.patcherModOutputSamples;
  auto& modLinks = runtime.patcherModLinks;
  auto& nodeToDeviceId = runtime.patcherNodeToDeviceId;
  auto& euclidOverrides = runtime.patcherEuclidOverrides;
  auto& hasEuclidOverride = runtime.patcherHasEuclidOverride;

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
}

}  // namespace daw::engine
