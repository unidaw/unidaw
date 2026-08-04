// Tests for reconcileChildTracks in apps/engine_track_setup.h — which aux CHILD tracks a
// multi-out parent should have.
//
// A multi-out instrument's extra outputs appear in the tracker as child tracks, one per bus. This
// function decides how many exist and creates or retires them to match, and the consumer thread
// calls it on EVERY tick — which is what makes its idempotence load-bearing rather than tidy. If
// the "does this child already exist" test ever stopped matching, every tick would append another
// child: no error, no crash, a project that grows tracks while you watch it and hits the 64-track
// budget on its own.
//
// THE SAMPLER PATH IS THE ONE WITH HISTORY. `requestBusLayout` asks the HOST what buses it has,
// and an in-engine sampler has no host to ask — so a track whose only multi-out source is the
// sampler reports an empty mask and used to get no children at all. That was the gap S6 left. The
// buses are synthesised from `stemCount` instead, one stereo bus per stem, laid out in the aux
// plane exactly as a plugin's would be. These tests pin that synthesis: the count, the bus
// numbering, and the channel offsets.
#include "apps/engine_track_setup.h"

#include <atomic>
#include <cstdio>
#include <mutex>
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

struct Fixture {
  daw::HostConfig baseConfig;
  std::atomic<uint32_t> clipVersion{0};
  std::atomic<uint32_t> liveTrackCount{1};   // just the parent, at slot 0
  std::vector<std::unique_ptr<TrackRuntime>> tracks;
  std::mutex tracksMutex;
  int childrenMade = 0;
  std::vector<std::pair<uint32_t, uint32_t>> madeBuses;  // (busIndex, channelOffset)

  Fixture() {
    baseConfig.numChannelsOut = 2;
    auto p = std::make_unique<TrackRuntime>();
    p->trackId = 0;
    p->hostReady.store(true, std::memory_order_release);
    tracks.push_back(std::move(p));
  }

  TrackRuntime& parent() { return *tracks[0]; }

  // Give the parent N sampler stems. This is the multi-out source that has no host to ask.
  void withStems(uint8_t n) {
    auto snap = std::make_shared<daw::SamplerRender>();
    snap->state.stemCount = n;
    parent().samplerSnapshot = snap;
  }

  ChildTrackDeps deps() {
    return ChildTrackDeps{
        baseConfig,
        [](const Track&) { return std::shared_ptr<const TrackStateSnapshot>{}; },
        clipVersion,
        liveTrackCount,
        [](TrackRuntime&) {},                                   // resetTrackContent
        [this](uint32_t childId, uint32_t parentTrackId, uint32_t busIndex,
               uint32_t busChannelOffset, uint32_t /*busChannelCount*/,
               const std::string& /*name*/) -> std::unique_ptr<TrackRuntime> {
          ++childrenMade;
          madeBuses.emplace_back(busIndex, busChannelOffset);
          auto rt = std::make_unique<TrackRuntime>();
          rt->trackId = childId;
          rt->isAuxChild.store(true, std::memory_order_release);
          rt->auxParentTrackId.store(parentTrackId, std::memory_order_release);
          rt->auxBusIndex.store(busIndex, std::memory_order_release);
          return rt;
        },
        tracks,
        tracksMutex};
  }

  // Stand in for what the caller does with the returned runtime: install it and count it live.
  void install() {
    // reconcileChildTracks places children itself; this only keeps liveTrackCount honest so a
    // second call sees the same world the engine would.
    liveTrackCount.store(static_cast<uint32_t>(1 + childrenMade), std::memory_order_release);
  }
};

// ------------------------------------- a sampler with stems gets one child per stem, no host asked
void testSamplerStemsSynthesiseChildren() {
  Fixture f;
  f.withStems(3);
  auto d = f.deps();
  reconcileChildTracks(d, f.parent());
  CHECK(f.childrenMade == 3);
  // Bus 0 is the main output, so stems start at 1. The offset handed to the child is the offset
  // WITHIN THE AUX PLANE, not the absolute channel: the synthesised bus sits at
  // numChannelsOut + i*2 and the main channels are subtracted back off before the child sees it.
  // I asserted the absolute value first and this test said so — worth keeping the distinction
  // written down, because the two agree at numChannelsOut == 0 and nowhere else.
  CHECK(f.madeBuses.size() == 3);
  if (f.madeBuses.size() == 3) {
    CHECK(f.madeBuses[0] == std::make_pair(1u, 0u));
    CHECK(f.madeBuses[1] == std::make_pair(2u, 2u));
    CHECK(f.madeBuses[2] == std::make_pair(3u, 4u));
  }
}

// ------------------------- CALLED AGAIN, IT CREATES NOTHING. The consumer calls this every tick.
void testReconcileIsIdempotent() {
  Fixture f;
  f.withStems(2);
  auto d = f.deps();
  reconcileChildTracks(d, f.parent());
  CHECK(f.childrenMade == 2);
  f.install();
  reconcileChildTracks(d, f.parent());
  reconcileChildTracks(d, f.parent());
  CHECK(f.childrenMade == 2);  // still 2, not 4 and not 6
}

// ----------------------------------------------- no stems and no host buses means no children
void testNoBusesNoChildren() {
  Fixture f;
  auto d = f.deps();
  reconcileChildTracks(d, f.parent());
  CHECK(f.childrenMade == 0);
}

// ----------------------------------- a host that has not reported yet is not a project with none
void testHostNotReadyCreatesNothing() {
  Fixture f;
  f.withStems(2);
  f.parent().hostReady.store(false, std::memory_order_release);
  auto d = f.deps();
  reconcileChildTracks(d, f.parent());
  // Not "this track has no buses" — "we have not been told yet". Creating zero children here
  // and remembering it would be a decision made from missing data.
  CHECK(f.childrenMade == 0);
}

// --------------------------------------- a child never spawns children of its own (no recursion)
void testAuxChildIsNotAParent() {
  Fixture f;
  f.withStems(2);
  f.parent().isAuxChild.store(true, std::memory_order_release);
  auto d = f.deps();
  reconcileChildTracks(d, f.parent());
  CHECK(f.childrenMade == 0);
}

}  // namespace

int main() {
  testSamplerStemsSynthesiseChildren();
  testReconcileIsIdempotent();
  testNoBusesNoChildren();
  testHostNotReadyCreatesNothing();
  testAuxChildIsNotAParent();
  if (g_fail == 0) {
    std::printf("engine_track_setup_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
