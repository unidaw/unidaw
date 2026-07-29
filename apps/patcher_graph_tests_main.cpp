#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

#include "apps/patcher_abi.h"
#include "apps/patcher_assemble.h"
#include "apps/patcher_graph.h"
#include "apps/patcher_preset.h"
#include "apps/patcher_preset_library.h"

namespace {

constexpr uint32_t kBufferCapacity = 256;
constexpr uint64_t kNanoticksPerQuarter = 960000;

struct NodeBuffer {
  std::array<daw::EventEntry, kBufferCapacity> events{};
  uint32_t count = 0;
};

uint32_t countDegreeEvents(const NodeBuffer& buffer) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < buffer.count; ++i) {
    const auto& entry = buffer.events[i];
    if (entry.type != static_cast<uint16_t>(daw::EventType::MusicalLogic)) {
      continue;
    }
    daw::MusicalLogicPayload payload{};
    std::memcpy(&payload, entry.payload, sizeof(payload));
    if (payload.metadata[0] == daw::kMusicalLogicKindDegree) {
      ++count;
    }
  }
  return count;
}

// Run ONE euclidean node with a given config and hand back everything it emitted, so a
// test can look at the payload fields rather than only counting events.
//
// This exists because four of euclidean's seven editable fields — degree,
// octave_offset, velocity, base_octave — were in the config, stored, round-tripped
// through the read-back and drawn as turnable controls, while the DSP hard-zeroed them
// into the payload. Nothing here could see it: the graph tests counted events, and an
// event with degree 0 is still an event.
std::vector<daw::MusicalLogicPayload> runEuclideanOnce(
    const daw::PatcherEuclideanConfig& config) {
  NodeBuffer buffer;
  uint64_t overflowTick = 0;
  daw::PatcherContext ctx{};
  ctx.abi_version = daw::kPatcherAbiVersion;
  ctx.block_start_tick = 0;
  ctx.block_end_tick = kNanoticksPerQuarter * 4;
  ctx.block_start_sample = 0;
  ctx.sample_rate = 48000.0f;
  ctx.tempo_bpm = 120.0f;
  ctx.num_frames = 512;
  ctx.event_buffer = buffer.events.data();
  ctx.event_capacity = static_cast<uint32_t>(buffer.events.size());
  ctx.event_count = &buffer.count;
  ctx.last_overflow_tick = &overflowTick;
  ctx.node_config = &config;
  ctx.node_config_size = sizeof(config);
  assert(daw::patcher_process_euclidean != nullptr);
  daw::patcher_process_euclidean(&ctx);

  std::vector<daw::MusicalLogicPayload> out;
  for (uint32_t i = 0; i < buffer.count; ++i) {
    const auto& entry = buffer.events[i];
    if (entry.type != static_cast<uint16_t>(daw::EventType::MusicalLogic)) {
      continue;
    }
    daw::MusicalLogicPayload payload{};
    std::memcpy(&payload, entry.payload, sizeof(payload));
    out.push_back(payload);
  }
  return out;
}

void testEuclideanConfigReachesThePayload() {
  daw::PatcherEuclideanConfig config{};
  config.steps = 8;
  config.hits = 3;
  config.degree = 5;
  config.octave_offset = -2;
  config.velocity = 77;
  config.base_octave = 6;
  const auto events = runEuclideanOnce(config);
  assert(events.size() == 3);  // one bar, 8 steps, 3 hits
  for (const auto& payload : events) {
    assert(payload.degree == 5);
    assert(payload.octave_offset == -2);
    assert(payload.velocity == 77);
    assert(payload.base_octave == 6);
  }

  // 0 MEANS 0. `hits 0` used to be read as "unset, use the default 5", so a generator
  // the user had silenced went on playing five notes a bar while the read-back
  // cheerfully reported 0. Defaults come from the config struct now, applied where the
  // file is parsed, so a zero that reaches the DSP was actually asked for.
  daw::PatcherEuclideanConfig silent = config;
  silent.hits = 0;
  assert(runEuclideanOnce(silent).empty());

  // Zero steps is not a pattern; it must not divide by zero either.
  daw::PatcherEuclideanConfig noSteps = config;
  noSteps.steps = 0;
  assert(runEuclideanOnce(noSteps).empty());

  std::cout << "euclidean_config_reaches_dsp: ok" << std::endl;
}

uint32_t runGraphOnce(const daw::PatcherGraph& graph) {
  const uint64_t blockStart = 0;
  const uint64_t blockEnd = kNanoticksPerQuarter * 4;
  std::vector<NodeBuffer> buffers(graph.nodes.size());
  uint64_t overflowTick = 0;

  auto runNode = [&](uint32_t nodeIndex) {
    if (nodeIndex >= graph.nodes.size()) {
      return;
    }
    auto& buffer = buffers[nodeIndex];
    buffer.count = 0;
    for (uint32_t inputIndex : graph.resolvedInputs[nodeIndex]) {
      if (inputIndex >= buffers.size()) {
        continue;
      }
      const auto& input = buffers[inputIndex];
      for (uint32_t i = 0; i < input.count && buffer.count < buffer.events.size(); ++i) {
        buffer.events[buffer.count++] = input.events[i];
      }
    }
    daw::PatcherContext ctx{};
    ctx.abi_version = daw::kPatcherAbiVersion;
    ctx.block_start_tick = blockStart;
    ctx.block_end_tick = blockEnd;
    ctx.block_start_sample = 0;
    ctx.sample_rate = 48000.0f;
    ctx.tempo_bpm = 120.0f;
    ctx.num_frames = 512;
    ctx.event_buffer = buffer.events.data();
    ctx.event_capacity = static_cast<uint32_t>(buffer.events.size());
    ctx.event_count = &buffer.count;
    ctx.last_overflow_tick = &overflowTick;

    const auto& node = graph.nodes[nodeIndex];
    if (node.type == daw::PatcherNodeType::Euclidean && node.hasEuclideanConfig) {
      ctx.node_config = &node.euclideanConfig;
      ctx.node_config_size = sizeof(node.euclideanConfig);
    } else if (node.type == daw::PatcherNodeType::RandomDegree && node.hasRandomDegreeConfig) {
      ctx.node_config = &node.randomDegreeConfig;
      ctx.node_config_size = sizeof(node.randomDegreeConfig);
    }

    switch (node.type) {
      case daw::PatcherNodeType::Euclidean:
        if (daw::patcher_process_euclidean) {
          daw::patcher_process_euclidean(&ctx);
        }
        break;
      case daw::PatcherNodeType::RandomDegree:
        if (daw::patcher_process_random_degree) {
          daw::patcher_process_random_degree(&ctx);
        }
        break;
      case daw::PatcherNodeType::Passthrough:
        if (daw::patcher_process_passthrough) {
          daw::patcher_process_passthrough(&ctx);
        }
        break;
      case daw::PatcherNodeType::EventOut:
        if (daw::patcher_process_event_out) {
          daw::patcher_process_event_out(&ctx);
        }
        break;
      default:
        break;
    }
  };

  for (uint32_t nodeIndex : graph.topoOrder) {
    runNode(nodeIndex);
  }

  for (size_t i = 0; i < graph.nodes.size(); ++i) {
    if (graph.nodes[i].type == daw::PatcherNodeType::EventOut) {
      return countDegreeEvents(buffers[i]);
    }
  }
  return 0;
}

daw::Device makePatcherDevice(uint32_t id, daw::DeviceKind kind,
                              const daw::PatcherGraph& graph) {
  daw::Device d;
  d.id = id;
  d.kind = kind;
  d.patcher = graph;
  return d;
}

// Builds a small device graph: euclid(0) -> eventOut(1). Output node is eventOut.
daw::PatcherGraph euclidToEventOut() {
  daw::PatcherGraph g;
  daw::PatcherNode euclid;
  euclid.id = 0;
  euclid.type = daw::PatcherNodeType::Euclidean;
  daw::PatcherNode out;
  out.id = 1;
  out.type = daw::PatcherNodeType::EventOut;
  g.nodes = {euclid, out};
  daw::PatcherEdge e;
  e.src = {0, daw::kPatcherEventOutputPort};
  e.dst = {1, daw::kPatcherEventInputPort};
  e.kind = daw::PatcherPortKind::Event;
  g.edges = {e};
  return g;
}

void testAssemblePatcherPool() {
  using namespace daw;

  // Two patcher devices, each with its own graph, merge into one pool with
  // globally unique ids and one output node reported per device. Device A names
  // its output explicitly (patcherNodeId = the EventOut, id 1).
  Device a = makePatcherDevice(10, DeviceKind::PatcherEvent, euclidToEventOut());
  a.patcherNodeId = 1;
  PatcherGraph single;  // one passthrough, no edges -> it is the sink/output
  PatcherNode pass;
  pass.id = 0;
  pass.type = PatcherNodeType::Passthrough;
  single.nodes = {pass};
  Device b = makePatcherDevice(20, DeviceKind::PatcherInstrument, single);

  AssembledPatcher merged = assemblePatcherPool({a, b});
  assert(merged.anyPerDevice);
  assert(merged.pool.nodes.size() == 3);  // 2 from A, 1 from B
  assert(merged.pool.edges.size() == 1);
  // Node ids are unique and contiguous across the two devices.
  assert(merged.pool.nodes[0].id == 0);
  assert(merged.pool.nodes[1].id == 1);
  assert(merged.pool.nodes[2].id == 2);
  assert(merged.deviceOutputs.size() == 2);
  assert(merged.deviceOutputs[0].first == 10 && merged.deviceOutputs[0].second == 1);
  assert(merged.deviceOutputs[1].first == 20 && merged.deviceOutputs[1].second == 2);
  // The remapped edge points at the merged ids (0 -> 1), not the originals.
  assert(merged.pool.edges[0].src.nodeId == 0);
  assert(merged.pool.edges[0].dst.nodeId == 1);
  // The merged pool builds cleanly and every device output resolves to a node.
  assert(buildPatcherGraph(merged.pool));
  for (const auto& [deviceId, outNode] : merged.deviceOutputs) {
    (void)deviceId;
    assert(outNode < merged.pool.idToIndex.size());
    assert(merged.pool.idToIndex[outNode] != kPatcherInvalidNodeIndex);
  }

  // Non-patcher devices, bypassed devices, and empty graphs are skipped.
  Device vst = makePatcherDevice(30, DeviceKind::VstInstrument, euclidToEventOut());
  Device bypassed = makePatcherDevice(40, DeviceKind::PatcherEvent, euclidToEventOut());
  bypassed.bypass = true;
  Device empty = makePatcherDevice(50, DeviceKind::PatcherEvent, PatcherGraph{});
  AssembledPatcher none = assemblePatcherPool({vst, bypassed, empty});
  assert(!none.anyPerDevice);
  assert(none.pool.nodes.empty());
  assert(none.deviceOutputs.empty());

  // When a device names no valid output node (patcherNodeId not in its graph),
  // assembly falls back to the graph's natural output (here the EventOut, id 1).
  Device c = makePatcherDevice(60, DeviceKind::PatcherEvent, euclidToEventOut());
  c.patcherNodeId = 999;  // not a node in its graph
  AssembledPatcher fb = assemblePatcherPool({c});
  assert(fb.deviceOutputs.size() == 1);
  assert(fb.deviceOutputs[0].first == 60);
  assert(fb.pool.nodes[fb.deviceOutputs[0].second].type == PatcherNodeType::EventOut);

  std::cout << "assemble_patcher_pool: ok" << std::endl;
}

}  // namespace

int main() {
  testAssemblePatcherPool();
  testEuclideanConfigReachesThePayload();

  daw::PatcherGraphState state;

  const uint32_t a = daw::addPatcherNode(state, daw::PatcherNodeType::Euclidean);
  const uint32_t b = daw::addPatcherNode(state, daw::PatcherNodeType::Passthrough);
  assert(a != std::numeric_limits<uint32_t>::max());
  assert(b != std::numeric_limits<uint32_t>::max());
  assert(a == 0);
  assert(b == 1);

  auto connected =
      daw::connectPatcherNodes(state,
                               a,
                               daw::kPatcherEventOutputPort,
                               b,
                               daw::kPatcherEventInputPort,
                               daw::PatcherPortKind::Event);
  assert(connected == daw::PatcherConnectResult::Ok);

  daw::PatcherEuclideanConfig config{};
  config.steps = 8;
  config.hits = 3;
  config.degree = 2;
  config.velocity = 80;
  const bool setConfig = daw::setEuclideanConfig(state, a, config);
  assert(setConfig);

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    assert(state.graph.nodes.size() == 2);
    assert(state.graph.topoOrder.size() == 2);
    assert(state.graph.depths.size() == 2);
    assert(state.graph.depths[0] == 0);
    assert(state.graph.depths[1] == 1);
    assert(state.graph.maxDepth == 1);
  }

  const bool removed = daw::removePatcherNode(state, a);
  assert(removed);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    assert(state.graph.nodes.size() == 1);
    assert(state.graph.nodes[0].id == b);
  }
  const uint32_t c = daw::addPatcherNode(state, daw::PatcherNodeType::Lfo);
  assert(c != std::numeric_limits<uint32_t>::max());
  assert(c != a);
  assert(c != b);

  const uint32_t d = daw::addPatcherNode(state, daw::PatcherNodeType::RustKernel);
  assert(d != std::numeric_limits<uint32_t>::max());
  assert(d != c);
  assert(d != b);
  assert(d != a);

  assert(daw::connectPatcherNodes(state,
                                  c,
                                  daw::kPatcherControlOutputPort,
                                  d,
                                  daw::kPatcherControlInputPort,
                                  daw::PatcherPortKind::Control) ==
         daw::PatcherConnectResult::InvalidConnection);

  const uint32_t audioIn = daw::addPatcherNode(state, daw::PatcherNodeType::AudioPassthrough);
  const uint32_t audioOut = daw::addPatcherNode(state, daw::PatcherNodeType::AudioPassthrough);
  assert(audioIn != std::numeric_limits<uint32_t>::max());
  assert(audioOut != std::numeric_limits<uint32_t>::max());
  assert(daw::connectPatcherNodes(state,
                                  audioOut,
                                  daw::kPatcherAudioOutputPort,
                                  audioIn,
                                  daw::kPatcherAudioInputPort,
                                  daw::PatcherPortKind::Audio) ==
         daw::PatcherConnectResult::Ok);

  {
    daw::PatcherGraphState saveState;
    const uint32_t e = daw::addPatcherNode(saveState, daw::PatcherNodeType::Euclidean);
    const uint32_t r = daw::addPatcherNode(saveState, daw::PatcherNodeType::RandomDegree);
    const uint32_t l = daw::addPatcherNode(saveState, daw::PatcherNodeType::Lfo);
    const uint32_t p = daw::addPatcherNode(saveState, daw::PatcherNodeType::Passthrough);
    assert(e == 0);
    assert(r == 1);
    assert(l == 2);
    assert(p == 3);
    assert(daw::connectPatcherNodes(saveState,
                                    e,
                                    daw::kPatcherEventOutputPort,
                                    r,
                                    daw::kPatcherEventInputPort,
                                    daw::PatcherPortKind::Event) ==
           daw::PatcherConnectResult::Ok);
    assert(daw::connectPatcherNodes(saveState,
                                    r,
                                    daw::kPatcherEventOutputPort,
                                    p,
                                    daw::kPatcherEventInputPort,
                                    daw::PatcherPortKind::Event) ==
           daw::PatcherConnectResult::Ok);
    assert(daw::connectPatcherNodes(saveState,
                                    l,
                                    daw::kPatcherEventOutputPort,
                                    p,
                                    daw::kPatcherEventInputPort,
                                    daw::PatcherPortKind::Event) ==
           daw::PatcherConnectResult::InvalidPort);

    daw::PatcherEuclideanConfig eCfg{};
    eCfg.steps = 12;
    eCfg.hits = 7;
    eCfg.degree = 3;
    eCfg.velocity = 72;
    assert(daw::setEuclideanConfig(saveState, e, eCfg));

    daw::PatcherRandomDegreeConfig rCfg{};
    rCfg.degree = 6;
    rCfg.velocity = 96;
    rCfg.duration_ticks = 120;
    assert(daw::setRandomDegreeConfig(saveState, r, rCfg));

    daw::PatcherLfoConfig lCfg{};
    lCfg.frequency_hz = 2.5f;
    lCfg.depth = 0.75f;
    lCfg.bias = 0.1f;
    lCfg.phase_offset = 0.25f;
    assert(daw::setLfoConfig(saveState, l, lCfg));

    const std::filesystem::path presetPath =
        std::filesystem::current_path() / "patcher_preset_test.json";
    std::string error;
    assert(daw::savePatcherPreset(saveState, presetPath.string(), &error));

    daw::PatcherGraphState loadState;
    assert(daw::loadPatcherPreset(loadState, presetPath.string(), &error));

    {
      std::lock_guard<std::mutex> lock(loadState.mutex);
      assert(loadState.graph.nodes.size() == 4);
      assert(loadState.graph.nodes[0].type == daw::PatcherNodeType::Euclidean);
      assert(loadState.graph.nodes[1].type == daw::PatcherNodeType::RandomDegree);
      assert(loadState.graph.nodes[2].type == daw::PatcherNodeType::Lfo);
      assert(loadState.graph.nodes[3].type == daw::PatcherNodeType::Passthrough);
      assert(loadState.graph.edges.size() == 2);
      assert(loadState.graph.edges[0].src.nodeId == e);
      assert(loadState.graph.edges[0].dst.nodeId == r);
      assert(loadState.graph.edges[0].src.portId == daw::kPatcherEventOutputPort);
      assert(loadState.graph.edges[0].dst.portId == daw::kPatcherEventInputPort);
      assert(loadState.graph.edges[0].kind == daw::PatcherPortKind::Event);
      assert(loadState.graph.edges[1].src.nodeId == r);
      assert(loadState.graph.edges[1].dst.nodeId == p);
      assert(loadState.graph.edges[1].src.portId == daw::kPatcherEventOutputPort);
      assert(loadState.graph.edges[1].dst.portId == daw::kPatcherEventInputPort);
      assert(loadState.graph.edges[1].kind == daw::PatcherPortKind::Event);
      assert(loadState.graph.nodes[0].euclideanConfig.steps == 12);
      assert(loadState.graph.nodes[0].euclideanConfig.hits == 7);
      assert(loadState.graph.nodes[0].euclideanConfig.degree == 3);
      assert(loadState.graph.nodes[0].euclideanConfig.velocity == 72);
      assert(loadState.graph.nodes[1].randomDegreeConfig.degree == 6);
      assert(loadState.graph.nodes[1].randomDegreeConfig.velocity == 96);
      assert(loadState.graph.nodes[1].randomDegreeConfig.duration_ticks == 120);
      assert(loadState.graph.nodes[2].lfoConfig.frequency_hz == 2.5f);
      assert(loadState.graph.nodes[2].lfoConfig.depth == 0.75f);
      assert(loadState.graph.nodes[2].lfoConfig.bias == 0.1f);
      assert(loadState.graph.nodes[2].lfoConfig.phase_offset == 0.25f);
      assert(loadState.graph.topoOrder.size() == 4);
    }
    std::error_code ec;
    std::filesystem::remove(presetPath, ec);
  }

  {
    const std::filesystem::path rootDir =
        std::filesystem::current_path() / "patcher_preset_scan_test";
    std::filesystem::create_directories(rootDir / "nested");
    std::ofstream(rootDir / "alpha.json").put('\n');
    std::ofstream(rootDir / "nested" / "beta.json").put('\n');
    std::ofstream(rootDir / "skip.txt").put('\n');

    std::vector<daw::PatcherPresetInfo> presets;
    std::string error;
    assert(daw::discoverPatcherPresets(rootDir.string(), presets, &error));
    assert(presets.size() == 2);
    assert(presets[0].name == "alpha");
    assert(presets[1].name == "beta");

    setenv("DAW_PATCHER_PRESET_DIR", rootDir.string().c_str(), 1);
    std::vector<std::string> names;
    assert(daw::listPatcherPresetNames(names, &error));
    assert(names.size() == 2);
    assert(names[0] == "alpha");
    assert(names[1] == "beta");

    std::error_code ec;
    std::filesystem::remove_all(rootDir, ec);
  }

  {
    std::filesystem::path presetPath =
        std::filesystem::current_path() / "presets" / "patcher" /
        "EuclideanRandomDegree.json";
    if (!std::filesystem::exists(presetPath)) {
      presetPath = std::filesystem::current_path() / ".." / "presets" / "patcher" /
          "EuclideanRandomDegree.json";
    }
    std::string error;
    daw::PatcherGraph graph;
    assert(std::filesystem::exists(presetPath));
    assert(daw::loadPatcherPreset(graph, presetPath.string(), &error));
    const uint32_t degreeEvents = runGraphOnce(graph);
    assert(degreeEvents > 0);
  }

  // graphHasEventGenerator: true iff a graph contains a generator node (euclidean/
  // random_degree). Drives the per-device kUiChainDiffGenerates flag the UI reads.
  {
    daw::PatcherGraph empty;
    assert(!daw::graphHasEventGenerator(empty));

    daw::PatcherGraph passthroughOnly;
    passthroughOnly.nodes.push_back({0, daw::PatcherNodeType::Passthrough});
    passthroughOnly.nodes.push_back({1, daw::PatcherNodeType::EventOut});
    assert(!daw::graphHasEventGenerator(passthroughOnly));

    daw::PatcherGraph lfoOnly;  // LFO is a modulation source, not an event generator
    lfoOnly.nodes.push_back({0, daw::PatcherNodeType::Lfo});
    assert(!daw::graphHasEventGenerator(lfoOnly));

    daw::PatcherGraph euclid;
    euclid.nodes.push_back({0, daw::PatcherNodeType::Euclidean});
    euclid.nodes.push_back({1, daw::PatcherNodeType::EventOut});
    assert(daw::graphHasEventGenerator(euclid));

    daw::PatcherGraph randomDegree;
    randomDegree.nodes.push_back({0, daw::PatcherNodeType::RandomDegree});
    randomDegree.nodes.push_back({1, daw::PatcherNodeType::EventOut});
    assert(daw::graphHasEventGenerator(randomDegree));
  }

  std::cout << "patcher_graph_tests_main: ok" << std::endl;
  return 0;
}
