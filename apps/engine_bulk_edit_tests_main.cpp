// Tests for handleAssembledBulk in apps/engine_bulk_edit.h — applying a reassembled bulk envelope.
//
// A bulk edit arrives as chunks and is reassembled before it gets here, which is exactly why the
// size checks matter: a carrier that delivers whatever arrived would hand this function a valid-
// LOOKING envelope with half its points. The code says so in a comment worth quoting, because it
// is the reason the rule is not obvious:
//
//     REFUSED, not truncated. An envelope with half its points is a VALID envelope, so a carrier
//     that delivered what arrived would produce a wrong sound rather than an error.
//
// That is the failure mode to protect: not a crash, not a rejection the user can see, but a sound
// that is subtly wrong and blamed on the instrument. Until this moved out of main() the only way to
// reach the branch was to boot an engine and deliberately corrupt a chunk stream.
#include "apps/engine_bulk_edit.h"

#include <atomic>
#include <cstdio>
#include <cstring>
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
  std::vector<std::unique_ptr<TrackRuntime>> tracks;
  std::mutex tracksMutex;
  std::atomic<bool> clipDirty{false};

  Fixture() {
    auto rt = std::make_unique<TrackRuntime>();
    rt->trackId = 0;
    daw::Device d;
    d.id = 1;
    d.kind = daw::DeviceKind::Sampler;
    rt->track.chain.devices.push_back(std::move(d));
    tracks.push_back(std::move(rt));
  }

  // Every callback is a no-op: what is under test is whether the ENVELOPE lands in the device,
  // not who gets told about it. Asserting on the callbacks would only prove the function called
  // the things it was handed.
  AssembledBulkDeps deps() {
    return AssembledBulkDeps{
        [](TrackRuntime*) -> uint32_t { return 0; },                       // bumpClipVersionFor
        clipDirty,
        [] {},                                                             // publishAudioClipTable
        [](const TrackRuntime&) { return std::shared_ptr<const AudioRenderList>{}; },
        [](TrackRuntime&) { return std::shared_ptr<const ClipSnapshot>{}; },
        [](TrackRuntime&) {},                                              // refreshSamplerForTrack
        [](daw::UiCommandType, daw::UiSamplerRejectReason, uint32_t, uint32_t, uint16_t) {},
        [](uint32_t, daw::UiCommandType, uint32_t) { return true; },
        [](const std::string& p) { return p; },                            // resolveSourcePath
        tracks,
        tracksMutex};
  }

  // How many envelope points the sampler device is currently holding, across every modulator.
  size_t envPointsHeld() const {
    size_t n = 0;
    for (const auto& d : tracks[0]->track.chain.devices) {
      for (const auto& ms : d.sampler.modSets) {
        for (const auto& m : ms.modulators) {
          n += m.env.points.size();
        }
      }
    }
    return n;
  }

  // COUNTING IS NOT ENOUGH, and the first version of this test was wrong for exactly that reason.
  // Applying an envelope calls ensureDefaultModSet, which mints a modulator carrying a DEFAULT
  // ADSR shape — and a default ADSR has four points. "Four points are held" was therefore true
  // whether or not a single byte of the payload had been read. So the assertions below look for
  // the VALUES that were sent: 1000, 900, 800, 700 at times 0, 100, 200, 300.
  bool holdsSentEnvelope() const {
    for (const auto& d : tracks[0]->track.chain.devices) {
      for (const auto& ms : d.sampler.modSets) {
        for (const auto& m : ms.modulators) {
          if (m.env.points.size() != 4) {
            continue;
          }
          bool match = true;
          for (size_t i = 0; i < 4; ++i) {
            if (m.env.points[i].time != static_cast<uint32_t>(i) * 100 ||
                m.env.points[i].valueMilli != static_cast<int16_t>(1000 - i * 100)) {
              match = false;
              break;
            }
          }
          if (match) {
            return true;
          }
        }
      }
    }
    return false;
  }
};

// Build an envelope envelope-points buffer carrying `declared` points but only `present` of them.
std::vector<uint8_t> envBuffer(uint16_t declared, uint16_t present) {
  daw::UiSamplerEnvPointsHeader h{};
  h.trackId = 0;
  h.deviceId = 1;
  h.pointCount = declared;
  // ADDRESS BY TARGET, not by modulator id. A fresh sampler device has no modulator id 0 to
  // address, so the first version of this fixture built a command the engine correctly refused —
  // and the count-based assertion passed anyway, because the default ADSR it minted on the way
  // also has four points. The command has to be VALID for the accept case to mean anything.
  h.flags = daw::kSamplerEnvByTarget;
  h.target = 0;  // Volume
  std::vector<uint8_t> buf(sizeof(h) + static_cast<size_t>(present) * sizeof(daw::UiEnvPointWire));
  std::memcpy(buf.data(), &h, sizeof(h));
  for (uint16_t i = 0; i < present; ++i) {
    daw::UiEnvPointWire w{};
    w.time = static_cast<uint32_t>(i) * 100;
    w.valueMilli = static_cast<int16_t>(1000 - i * 100);
    std::memcpy(buf.data() + sizeof(h) + static_cast<size_t>(i) * sizeof(w), &w, sizeof(w));
  }
  return buf;
}

// ------------------------------------------------------ a complete envelope is applied
void testCompleteEnvelopeApplies() {
  Fixture f;
  auto d = f.deps();
  CHECK(!f.holdsSentEnvelope());
  handleAssembledBulk(d, envBuffer(/*declared=*/4, /*present=*/4));
  CHECK(f.holdsSentEnvelope());
}

// ---------------- an envelope DECLARING more points than it carries is refused, not truncated
void testShortPayloadIsRefusedNotTruncated() {
  Fixture f;
  auto d = f.deps();
  handleAssembledBulk(d, envBuffer(/*declared=*/4, /*present=*/2));
  // Nothing applied. Applying the two that arrived would be a valid-looking envelope of the
  // wrong shape — audible, and impossible to distinguish from a bad patch.
  CHECK(!f.holdsSentEnvelope());
  CHECK(f.envPointsHeld() == 0);
}

// ------------------------------------------- an envelope of fewer than two points is refused
void testTooFewPointsIsRefused() {
  Fixture f;
  auto d = f.deps();
  handleAssembledBulk(d, envBuffer(/*declared=*/1, /*present=*/1));
  CHECK(f.envPointsHeld() == 0);
}

// --------------------------------------- a buffer too short to hold even the opcode is dropped
void testRuntBufferIsDropped() {
  Fixture f;
  auto d = f.deps();
  handleAssembledBulk(d, std::vector<uint8_t>{});
  handleAssembledBulk(d, std::vector<uint8_t>{7});
  CHECK(f.envPointsHeld() == 0);
}

}  // namespace

int main() {
  testCompleteEnvelopeApplies();
  testShortPayloadIsRefusedNotTruncated();
  testTooFewPointsIsRefused();
  testRuntBufferIsDropped();
  if (g_fail == 0) {
    std::printf("engine_bulk_edit_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
