#include <cassert>
#include <iostream>
#include <limits>

#include "apps/patcher_graph.h"

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
    assert(state.graph.nodes[0].id == 0);
  }

  std::cout << "patcher_graph_tests_main: ok" << std::endl;
  return 0;
}
