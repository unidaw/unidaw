#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "apps/patcher_graph.h"
#include "apps/patcher_preset.h"
#include "apps/patcher_preset_library.h"

int main() {
  daw::PatcherGraphState state;

  const uint32_t a = daw::addPatcherNode(state, daw::PatcherNodeType::Euclidean);
  const uint32_t b = daw::addPatcherNode(state, daw::PatcherNodeType::Passthrough);
  assert(a != std::numeric_limits<uint32_t>::max());
  assert(b != std::numeric_limits<uint32_t>::max());
  assert(a == 0);
  assert(b == 1);

  bool connected = daw::connectPatcherNodes(state, a, b);
  assert(connected);

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
    assert(daw::connectPatcherNodes(saveState, e, r));
    assert(daw::connectPatcherNodes(saveState, r, p));
    assert(daw::connectPatcherNodes(saveState, l, p));

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
      assert(loadState.graph.nodes[3].inputs.size() == 2);
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

  std::cout << "patcher_graph_tests_main: ok" << std::endl;
  return 0;
}
