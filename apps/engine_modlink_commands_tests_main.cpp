// Tests for apps/engine_modlink_commands.h — the modulation-link commands.
//
// This module was 362 lines with no unit coverage at all. Every existing check drives modulation
// through daw-cli and asserts on what SOUNDS or on what is PUBLISHED, which can only see the happy
// path: a refusal on this wire is a silent no-op, and the caller gets {"sent": ...} either way.
// Six distinct refusal reasons live in handleAddModLink and nothing asserted on any of them.
//
// THE SAME-DEVICE CASE IS WHY THIS FILE EXISTS. handleAddModLink refuses a link that flows
// BACKWARD through the chain — a device modulating one earlier than itself, whose value does not
// exist yet by the time the earlier device's audio has gone past. That test used to be `>=`, which
// also refused a device modulating ITSELF, and self-modulation is the ordinary case now that
// patchers are per-device: an LFO in device N's own graph driving device N's cutoff.
//
// The cost of that off-by-one was not a rejected command. The LOADER installs mod links without
// this check, so the engine ACCEPTED from a file exactly what it REFUSED from the UI —
// presets/projects/rack.uniproj.json ships such a link, so the rack demo's modulation worked when
// loaded and could never be recreated by hand. It was found by daw_lint sweeping the presets, not
// by any test. Nothing has guarded it since. Now something does.
//
// Each test builds the handler's dependencies directly — a track table, a mutex, and stubs that
// RECORD what the handler asked for. No engine, no shared memory, no ring, no device.
#include "apps/engine_modlink_commands.h"

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

// The error codes handleAddModLink emits, restated. They are function-local constexprs inside the
// handler, so there is nothing to include and this IS a second copy — the shape this repo treats as
// its most expensive, two definitions agreeing on names and free to diverge in value.
//
// What keeps it honest is that every refusal test below asserts a SPECIFIC code rather than merely
// "an error was emitted". Renumber kModErrInvalidDevice in the handler and testUnknownDeviceRefused
// fails, because it is comparing against 4 and receiving something else. The duplication cannot be
// removed without lifting those constants into the header, which would be the better fix; until
// then the assertions are what stop the copy going stale silently.
constexpr uint16_t kErrTrackMissing = 1;
constexpr uint16_t kErrLinkMissing = 2;
constexpr uint16_t kErrInvalidKind = 3;
constexpr uint16_t kErrInvalidDevice = 4;
constexpr uint16_t kErrOrderViolation = 5;
constexpr uint16_t kErrLinkExists = 6;

struct Recorded {
  struct Err {
    uint16_t code;
    uint32_t trackId;
    uint32_t linkId;
  };
  std::vector<Err> errors;
  int snapshots = 0;
  std::vector<std::string> history;
};

// One object because ModlinkCommandDeps holds REFERENCES: every member must outlive it.
struct Fixture {
  std::vector<std::unique_ptr<TrackRuntime>> tracks;
  std::mutex tracksMutex;
  Recorded rec;

  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildSnapshotFn =
      [](const Track&) { return std::make_shared<const TrackStateSnapshot>(); };
  std::function<void(uint16_t, uint32_t, uint32_t)> emitModErrorFn =
      [this](uint16_t code, uint32_t trackId, uint32_t linkId) {
        rec.errors.push_back({code, trackId, linkId});
      };
  std::function<void(TrackRuntime&)> emitModSnapshotFn = [this](TrackRuntime&) {
    rec.snapshots++;
  };
  std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>
      historyAppendFn = [this](const char* op, const char*, uint32_t, uint32_t,
                               const std::string&) { rec.history.push_back(op ? op : ""); };

  ModlinkCommandDeps deps() {
    return ModlinkCommandDeps{tracks,          tracksMutex,       buildSnapshotFn,
                              emitModErrorFn,  emitModSnapshotFn, historyAppendFn};
  }

  // A track whose chain is the given device ids IN ORDER. Order is the whole subject of the
  // forward-flow rule, so the fixture has to make position observable rather than incidental.
  void addTrack(uint32_t trackId, std::vector<uint32_t> deviceIds) {
    auto rt = std::make_unique<TrackRuntime>();
    rt->trackId = trackId;
    for (uint32_t id : deviceIds) {
      daw::Device d;
      d.id = id;
      d.kind = daw::DeviceKind::Sampler;
      d.hasSampler = true;
      rt->track.chain.devices.push_back(d);
    }
    tracks.push_back(std::move(rt));
  }

  size_t linkCount(uint32_t trackId) const {
    for (const auto& rt : tracks) {
      if (rt->trackId == trackId) {
        return rt->track.modRegistry.links.size();
      }
    }
    return 0;
  }
};

daw::EventEntry entryOf(const daw::UiModLinkCommandPayload& p) {
  daw::EventEntry e{};
  e.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  e.size = sizeof(p);
  std::memcpy(e.payload, &p, sizeof(p));
  return e;
}

// flags: bits 0-3 source kind, 4-7 target kind, 8-9 rate, 10 enabled.
uint16_t flagsFor(uint8_t sourceKind, uint8_t targetKind, uint8_t rate, bool enabled) {
  return static_cast<uint16_t>((sourceKind & 0x0Fu) | ((targetKind & 0x0Fu) << 4) |
                               ((rate & 0x03u) << 8) | (enabled ? (1u << 10) : 0u));
}

daw::UiModLinkCommandPayload addPayload(uint32_t trackId, uint32_t srcDev, uint32_t dstDev) {
  daw::UiModLinkCommandPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::AddModLink);
  p.flags = flagsFor(0, 0, 0, true);
  p.trackId = trackId;
  p.linkId = 1;
  p.sourceDeviceId = srcDev;
  p.targetDeviceId = dstDev;
  p.depth = 0.5f;
  return p;
}

void run(Fixture& f, const daw::UiModLinkCommandPayload& p) {
  auto d = f.deps();
  const auto e = entryOf(p);
  daw::UiCommandPayload header{};
  daw::engine::handleAddModLink(d, e, header,
                                static_cast<daw::UiCommandType>(p.commandType));
}

// ---- A DEVICE MAY MODULATE ITSELF. The regression this file was written for.
void testSelfModulationAccepted() {
  Fixture f;
  f.addTrack(0, {10, 20});
  run(f, addPayload(0, 10, 10));
  CHECK(f.rec.errors.empty());
  CHECK(f.linkCount(0) == 1);
  CHECK(f.rec.snapshots == 1);
}

// ---- FORWARD IS ALLOWED, and this is the control for the case above: if both passed for the
// same reason, "self is allowed" would prove nothing about the ordering rule.
void testForwardLinkAccepted() {
  Fixture f;
  f.addTrack(0, {10, 20});
  run(f, addPayload(0, 10, 20));
  CHECK(f.rec.errors.empty());
  CHECK(f.linkCount(0) == 1);
}

// ---- BACKWARD IS REFUSED, and nothing is stored. Asserting the link count is what separates a
// refusal from a rejection-plus-write, which is the failure that looks like it worked.
void testBackwardLinkRefused() {
  Fixture f;
  f.addTrack(0, {10, 20});
  run(f, addPayload(0, 20, 10));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrOrderViolation);
  CHECK(f.linkCount(0) == 0);
  CHECK(f.rec.snapshots == 0);
}

void testMissingTrackRefused() {
  Fixture f;
  f.addTrack(0, {10});
  run(f, addPayload(7, 10, 10));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrTrackMissing);
  CHECK(f.linkCount(0) == 0);
}

void testUnknownDeviceRefused() {
  Fixture f;
  f.addTrack(0, {10, 20});
  run(f, addPayload(0, 10, 99));
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidDevice);
  CHECK(f.linkCount(0) == 0);
}

// ---- AN OUT-OF-RANGE KIND IS REFUSED. 0x0F is above every ModSourceKind, and the decoder returns
// nullopt rather than casting an integer into an enum nobody defined.
void testInvalidKindRefused() {
  Fixture f;
  f.addTrack(0, {10});
  auto p = addPayload(0, 10, 10);
  p.flags = flagsFor(0x0F, 0, 0, true);
  run(f, p);
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrInvalidKind);
  CHECK(f.linkCount(0) == 0);
}

// ---- A DUPLICATE IS REFUSED RATHER THAN DOUBLED. Two identical links would both modulate the
// same target and the depths would sum, which reads as "the knob is twice as sensitive".
void testDuplicateRefused() {
  Fixture f;
  f.addTrack(0, {10, 20});
  run(f, addPayload(0, 10, 20));
  CHECK(f.linkCount(0) == 1);
  run(f, addPayload(0, 10, 20));
  CHECK(f.linkCount(0) == 1);
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrLinkExists);
}

// ---- A COMMAND THIS HANDLER DOES NOT OWN IS IGNORED IN SILENCE, and that is correct: the
// dispatcher may route several opcodes here and the handler filters. It must not emit an error for
// one it simply is not for — an error would be indistinguishable from a real refusal to a caller.
void testForeignCommandIgnored() {
  Fixture f;
  f.addTrack(0, {10});
  auto p = addPayload(0, 10, 10);
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::SetTrackName);
  run(f, p);
  CHECK(f.rec.errors.empty());
  CHECK(f.rec.snapshots == 0);
  CHECK(f.linkCount(0) == 0);
}

// ---- REMOVING A LINK THAT IS NOT THERE IS A REFUSAL, not a no-op that reports success.
void testRemoveMissingLinkRefused() {
  Fixture f;
  f.addTrack(0, {10, 20});
  auto p = addPayload(0, 10, 20);
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::RemoveModLink);
  p.linkId = 42;
  run(f, p);
  CHECK(f.rec.errors.size() == 1);
  CHECK(!f.rec.errors.empty() && f.rec.errors[0].code == kErrLinkMissing);
}

// ---- AND A REMOVE OF A REAL LINK DOES NOT NEED THE DEVICE IDS. A remove is addressed by
// (track, link); it used to fall through the ADD's device validation, so a caller that knew the
// link's id still had to supply the devices it happens to connect, and unstated ids default to 0.
void testRemoveNeedsOnlyTrackAndLink() {
  Fixture f;
  f.addTrack(0, {10, 20});
  run(f, addPayload(0, 10, 20));
  CHECK(f.linkCount(0) == 1);
  daw::UiModLinkCommandPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::RemoveModLink);
  p.trackId = 0;
  p.linkId = 1;
  // sourceDeviceId and targetDeviceId deliberately left 0 — no such device exists.
  run(f, p);
  CHECK(f.rec.errors.empty());
  CHECK(f.linkCount(0) == 0);
}

}  // namespace

int main() {
  testSelfModulationAccepted();
  testForwardLinkAccepted();
  testBackwardLinkRefused();
  testMissingTrackRefused();
  testUnknownDeviceRefused();
  testInvalidKindRefused();
  testDuplicateRefused();
  testForeignCommandIgnored();
  testRemoveMissingLinkRefused();
  testRemoveNeedsOnlyTrackAndLink();

  if (g_fail == 0) {
    std::printf("engine_modlink_commands_tests: PASS — 10 cases, six refusal reasons and the\n");
    std::printf("                               self-modulation rule that a preset could load\n");
    std::printf("                               and no command could recreate\n");
  }
  return g_fail == 0 ? 0 : 1;
}
