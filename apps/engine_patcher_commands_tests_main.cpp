// Tests for apps/engine_patcher_commands.h — the shared-pool patcher graph commands.
//
// 521 lines, six distinct refusal codes, and no unit coverage. Every existing patcher check drives
// the graph through daw-cli and then asserts on what is PUBLISHED or what SOUNDS, which sees only
// the accepted path: a refusal here emits a structured error and returns, and the caller is told
// {"sent": ...} either way.
//
// THE CYCLE IS THE ONE THAT MATTERS. A patcher graph is executed in topological order, so an edge
// that closes a loop has no valid order to run in. Unlike the other refusals, its cost is not a
// missing feature — it is a graph the RT path has to walk, and the failure surfaces as a hang or a
// depth-capped run rather than as anything a user could connect to what they just clicked. It is
// also the refusal least likely to be exercised by hand, because you have to be trying.
//
// SELF-CONNECTION IS ITS CONTROL. A -> A is refused by a DIFFERENT branch (a direct src == dst
// test, before the graph is consulted at all) and reports a different code. Testing only one of
// them would leave "the handler refuses something" indistinguishable from "the handler refuses the
// right thing for the right reason".
//
// Each test builds the deps directly — a PatcherGraphOwner and stubs that RECORD. No engine, no
// shared memory, no ring, no device, no track needed for the pool path.
#include "apps/engine_patcher_commands.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace daw;
using namespace daw::engine;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

// The handler's error codes, restated — they are function-local constexprs, so there is nothing to
// include. Every test below asserts a SPECIFIC one rather than "an error happened", which is what
// makes a renumbering in the handler fail here instead of passing against the old meaning.
constexpr uint16_t kErrInvalidType = 1;
constexpr uint16_t kErrInvalidNode = 2;
constexpr uint16_t kErrCycle = 3;
constexpr uint16_t kErrInvalidConnection = 5;

struct Recorded {
  struct Err {
    uint16_t code;
    uint32_t srcNodeId;
    uint32_t dstNodeId;
  };
  std::vector<Err> errors;
  int deltas = 0;
  int snapshots = 0;
};

struct Fixture {
  // ONE OBJECT NOW. trackTable.tracks and trackTable.tracksMutex were never apart in any interface, so they are a
  // TrackTable; the handler takes it whole and the fixture builds it whole.
  daw::engine::EngineState engineState;
  TrackTable& trackTable = engineState.trackTable;
  // ONE OBJECT NOW, not four separate locals. PatcherGraphOwner holds the pool, the RT snapshot
  // and the two flags; the handler takes it whole, so the fixture builds it whole.
  PatcherGraphOwner& patcherGraph = engineState.patcherGraph;
  Recorded rec;

  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildSnapshotFn =
      [](const Track&) { return std::make_shared<const TrackStateSnapshot>(); };
  std::function<void(uint32_t, uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t, uint32_t)>
      emitDeltaFn = [this](uint32_t, uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t) { rec.deltas++; };
  std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t)>
      emitErrorFn = [this](uint16_t code, uint32_t, uint32_t, uint32_t src, uint32_t dst,
                           uint32_t, uint32_t, uint32_t) {
        rec.errors.push_back({code, src, dst});
      };
  std::function<void(const daw::UiDiffPayload&)> emitUiDiffFn = [](const daw::UiDiffPayload&) {};
  std::function<bool()> reassembleFn = []() { return true; };
  std::function<void()> updateSnapshotFn = [this]() { rec.snapshots++; };

  PatcherCommandDeps deps() {
    return PatcherCommandDeps{engineState, buildSnapshotFn,
                              emitDeltaFn,     emitErrorFn, emitUiDiffFn, reassembleFn,
                              updateSnapshotFn};
  }

  size_t nodeCount() {
    std::lock_guard<std::mutex> lock(patcherGraph.patcherGraphState.mutex);
    return patcherGraph.patcherGraphState.graph.nodes.size();
  }
  size_t edgeCount() {
    std::lock_guard<std::mutex> lock(patcherGraph.patcherGraphState.mutex);
    return patcherGraph.patcherGraphState.graph.edges.size();
  }
};

daw::EventEntry entryOf(const daw::UiPatcherGraphCommandPayload& p) {
  daw::EventEntry e{};
  e.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  e.size = sizeof(p);
  std::memcpy(e.payload, &p, sizeof(p));
  return e;
}

void run(Fixture& f, const daw::UiPatcherGraphCommandPayload& p) {
  auto d = f.deps();
  const auto e = entryOf(p);
  daw::UiCommandPayload header{};
  daw::engine::handleAddPatcherNode(d, e, header,
                                    static_cast<daw::UiCommandType>(p.commandType));
}

daw::UiPatcherGraphCommandPayload addNode(uint32_t nodeType) {
  daw::UiPatcherGraphCommandPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::AddPatcherNode);
  p.nodeType = nodeType;
  return p;
}

daw::UiPatcherGraphCommandPayload connect(uint32_t src, uint32_t dst, uint32_t edgeKind = 0) {
  daw::UiPatcherGraphCommandPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::ConnectPatcherNodes);
  p.srcNodeId = src;
  p.dstNodeId = dst;
  p.srcPortId = 1;
  p.dstPortId = 0;
  p.edgeKind = edgeKind;
  return p;
}

// Two nodes in the pool, returned by id, so the connection tests have something real to wire.
std::pair<uint32_t, uint32_t> twoNodes(Fixture& f) {
  run(f, addNode(static_cast<uint32_t>(daw::PatcherNodeType::Passthrough)));
  run(f, addNode(static_cast<uint32_t>(daw::PatcherNodeType::Passthrough)));
  std::lock_guard<std::mutex> lock(f.patcherGraph.patcherGraphState.mutex);
  const auto& nodes = f.patcherGraph.patcherGraphState.graph.nodes;
  return {nodes[0].id, nodes[1].id};
}

// ---- A CYCLE IS REFUSED. A -> B is fine; B -> A closes the loop and has no topological order.
void testCycleRefused() {
  Fixture f;
  const auto ids = twoNodes(f);
  run(f, connect(ids.first, ids.second));
  CHECK(f.rec.errors.empty());
  const size_t edgesAfterFirst = f.edgeCount();
  CHECK(edgesAfterFirst == 1);

  f.rec.errors.clear();
  run(f, connect(ids.second, ids.first));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrCycle);
  // AND THE EDGE IS NOT THERE. A refusal that still writes is the failure that looks like success,
  // and on this path it would leave the RT graph with a loop in it.
  CHECK(f.edgeCount() == edgesAfterFirst);
}

// ---- SELF-CONNECTION IS REFUSED BY A DIFFERENT BRANCH, with a different code: src == dst is
// tested directly, before the graph is consulted. Same outcome, different reason, and asserting
// the code is what tells them apart.
void testSelfConnectionRefused() {
  Fixture f;
  const auto ids = twoNodes(f);
  run(f, connect(ids.first, ids.first));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidNode);
  CHECK(f.edgeCount() == 0);
}

// ---- A FORWARD EDGE IS ACCEPTED. The control for both refusals above: without it, "the handler
// refuses cycles" would be satisfied by a handler that refuses everything.
void testForwardConnectionAccepted() {
  Fixture f;
  const auto ids = twoNodes(f);
  run(f, connect(ids.first, ids.second));
  CHECK(f.rec.errors.empty());
  CHECK(f.edgeCount() == 1);
  CHECK(f.rec.deltas > 0);
  CHECK(f.patcherGraph.patcherPoolEdited.load());
}

// ---- AN UNKNOWN NODE TYPE IS REFUSED rather than cast into an enum nobody defined.
void testInvalidNodeTypeRefused() {
  Fixture f;
  run(f, addNode(static_cast<uint32_t>(daw::kPatcherNodeTypeMax) + 1));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidType);
  CHECK(f.nodeCount() == 0);
}

// ---- THE LAST VALID TYPE IS ACCEPTED, which is the boundary the check above could get wrong in
// the other direction. kPatcherNodeTypeMax is SliceSelect; an off-by-one in the comparison would
// refuse the newest node type and nothing else would notice.
void testMaxNodeTypeAccepted() {
  Fixture f;
  run(f, addNode(static_cast<uint32_t>(daw::kPatcherNodeTypeMax)));
  CHECK(f.rec.errors.empty());
  CHECK(f.nodeCount() == 1);
}

// ---- AN OUT-OF-RANGE EDGE KIND IS REFUSED. Control = 2 is the largest; 3 is not a port kind.
void testInvalidEdgeKindRefused() {
  Fixture f;
  const auto ids = twoNodes(f);
  run(f, connect(ids.first, ids.second,
                 static_cast<uint32_t>(daw::PatcherPortKind::Control) + 1));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidConnection);
  CHECK(f.edgeCount() == 0);
}

// ---- CONNECTING A NODE THAT IS NOT THERE IS REFUSED, and does not add one on the way.
void testConnectUnknownNodeRefused() {
  Fixture f;
  const auto ids = twoNodes(f);
  run(f, connect(ids.first, 9999));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidNode);
  CHECK(f.edgeCount() == 0);
  CHECK(f.nodeCount() == 2);
}

// ---- REMOVING A NODE THAT IS NOT THERE IS A REFUSAL, not a silent success.
void testRemoveUnknownNodeRefused() {
  Fixture f;
  twoNodes(f);
  daw::UiPatcherGraphCommandPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::RemovePatcherNode);
  p.nodeId = 9999;
  run(f, p);
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidNode);
  CHECK(f.nodeCount() == 2);
}

// ---- REMOVING A REAL NODE WORKS, and republishes. Without this the test above would pass against
// a handler that refuses every remove.
void testRemoveRealNode() {
  Fixture f;
  const auto ids = twoNodes(f);
  daw::UiPatcherGraphCommandPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::RemovePatcherNode);
  p.nodeId = ids.first;
  run(f, p);
  CHECK(f.rec.errors.empty());
  CHECK(f.nodeCount() == 1);
  CHECK(f.rec.snapshots > 0);
}

}  // namespace

int main() {
  testCycleRefused();
  testSelfConnectionRefused();
  testForwardConnectionAccepted();
  testInvalidNodeTypeRefused();
  testMaxNodeTypeAccepted();
  testInvalidEdgeKindRefused();
  testConnectUnknownNodeRefused();
  testRemoveUnknownNodeRefused();
  testRemoveRealNode();

  if (g_fail == 0) {
    std::printf("engine_patcher_commands_tests: PASS — 9 cases; the cycle refusal, the\n");
    std::printf("                               self-connection that reports a DIFFERENT code,\n");
    std::printf("                               and the node-type boundary in both directions\n");
  }
  return g_fail == 0 ? 0 : 1;
}
