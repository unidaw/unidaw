// Tests for apps/engine_automation_commands.h.
//
// handleWriteAutomationPoint has FOUR refusal paths and every one of them is SILENT: it emits a
// structured event and returns, with nothing on the wire. A caller driving this through daw-cli
// sees {"sent": ...} and an automation lane that never appears. Two of the four document bugs
// that actually shipped:
//
//   param_id_not_representable — a 16-byte param id fills the field with no terminator. The
//       read-back slot nul-terminates inside its own 16 bytes, so the write and the answer would
//       name different lanes forever and nothing would report it.
//   track_not_persisted — `trackId < trackTable.tracks.size()` was once the only test, and it is true for a
//       tombstone, a leftover slot past the live count, and an aux child. Writing automation to
//       any of them was ACCEPTED and reported created_clip:true, and the points were gone after
//       the next save/reload with nothing having said no.
//
// So these tests assert the REASON, not just the absence of an effect. DAW_EVENT_LOG redirects
// structured events to a file and event_log.cpp fflushes after every write, so the test reads the
// bytes appended by one call and asserts which refusal fired. Without that, "no lane was created"
// is indistinguishable across all four paths — and a handler that refused for the WRONG reason
// would pass every one of them.
#include "apps/engine_automation_commands.h"

#include <cstdio>
#include <cstdlib>
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

const char* kLogPath = nullptr;

// Reads only what one call appended. Comparing against the whole file would let an earlier test's
// event satisfy a later assertion — the same "predicate matched something already there" trap
// that made a poll return instantly elsewhere in this repo.
struct EventTail {
  long mark = 0;
  void reset() { mark = sizeOf(); }
  std::string since() const {
    std::FILE* f = std::fopen(kLogPath, "rb");
    if (!f) return {};
    std::fseek(f, mark, SEEK_SET);
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
  }
  static long sizeOf() {
    std::FILE* f = std::fopen(kLogPath, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fclose(f);
    return n;
  }
};

struct Fixture {
  // ONE OBJECT NOW. trackTable.tracks and trackTable.tracksMutex were never apart in any interface, so they are a
  // TrackTable; the handler takes it whole and the fixture builds it whole.
  TrackTable trackTable;
  std::atomic<uint32_t> automationVersion{0};
  UiShmState shm;
  bool persisted = true;                       // what trackIsPersisted answers
  EventTail log;

  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildSnapFn =
      [](const Track&) { return std::make_shared<const TrackStateSnapshot>(); };
  std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>
      historyFn = [](const char*, const char*, uint32_t, uint32_t, const std::string&) {};
  std::function<bool(const TrackRuntime&)> persistedFn =
      [this](const TrackRuntime&) { return persisted; };
  std::function<bool(uint32_t, daw::UiCommandType, uint32_t)> versionFn =
      [](uint32_t, daw::UiCommandType, uint32_t) { return true; };

  AutomationCommandDeps deps() {
    return AutomationCommandDeps{trackTable, automationVersion, shm,
                                 buildSnapFn, historyFn,   persistedFn,       versionFn};
  }

  void addTrack() {
    auto rt = std::make_unique<TrackRuntime>();
    rt->trackId = static_cast<uint32_t>(trackTable.tracks.size());
    trackTable.tracks.push_back(std::move(rt));
  }

  size_t laneCount(uint32_t trackId) const {
    return trackTable.tracks[trackId]->track.automationClips.size();
  }
};

daw::UiAutomationPointPayload pointPayload(uint32_t trackId, const char* paramId,
                                           uint64_t tick = 0, float value = 0.5f) {
  daw::UiAutomationPointPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::WriteAutomationPoint);
  p.trackId = trackId;
  // THE ALL-TARGET SENTINEL, which is what every real sender defaults to (daw-cli's `--device`
  // and the sidecar's `"target"` both fall back to it).
  //
  // A zero-initialised payload leaves this 0, and 0 used to be a legal COMPACT HOST INDEX — "the
  // first hosted plugin". It is not a target at all now: a durable target names a device, and
  // zero is the absence of one (AE-P1.2 G2-B item 18, R-PROJECT-TARGET-MIGRATION). The handler
  // refuses it, so a payload that left it zero would test the refusal rather than the write.
  p.targetPluginIndex = daw::kParamTargetAll;
  p.nanotickLo = static_cast<uint32_t>(tick & 0xffffffffu);
  p.nanotickHi = static_cast<uint32_t>(tick >> 32);
  p.value = value;
  // Deliberately NOT strncpy-with-terminator: a 16-char id must be able to fill the field, which
  // is the case the representability guard exists for.
  std::memcpy(p.paramId, paramId, std::min(std::strlen(paramId), sizeof(p.paramId)));
  return p;
}

std::string callWrite(Fixture& f, const daw::UiAutomationPointPayload& p) {
  daw::EventEntry e{};
  e.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  e.size = sizeof(p);
  std::memcpy(e.payload, &p, sizeof(p));
  daw::UiCommandPayload header{};
  header.commandType = static_cast<uint16_t>(daw::UiCommandType::WriteAutomationPoint);
  auto deps = f.deps();
  f.log.reset();
  handleWriteAutomationPoint(deps, e, header, daw::UiCommandType::WriteAutomationPoint);
  return f.log.since();
}

// ------------------------------------------------------------------------- the four refusals
void testEmptyParamId() {
  Fixture f;
  f.addTrack();
  const std::string ev = callWrite(f, pointPayload(0, ""));
  CHECK(ev.find("empty_param_id") != std::string::npos);
  CHECK(f.laneCount(0) == 0);
}

void testParamIdNotRepresentable() {
  Fixture f;
  f.addTrack();
  // Exactly 16 characters: fills paramId with no room for a terminator.
  const std::string ev = callWrite(f, pointPayload(0, "0123456789abcdef"));
  CHECK(ev.find("param_id_not_representable") != std::string::npos);
  CHECK(f.laneCount(0) == 0);

  // FIFTEEN is the longest id that CAN round-trip, and it must be accepted. An off-by-one in the
  // guard would refuse it, and no test of "16 is refused" would notice.
  Fixture g;
  g.addTrack();
  const std::string ok = callWrite(g, pointPayload(0, "0123456789abcde"));
  CHECK(ok.find("param_id_not_representable") == std::string::npos);
  CHECK(g.laneCount(0) == 1);
}

void testNoSuchTrack() {
  Fixture f;                                   // no trackTable.tracks
  const std::string ev = callWrite(f, pointPayload(0, "cutoff"));
  CHECK(ev.find("no_such_track") != std::string::npos);

  Fixture g;
  g.addTrack();
  const std::string ev2 = callWrite(g, pointPayload(9, "cutoff"));   // past the end
  CHECK(ev2.find("no_such_track") != std::string::npos);
  CHECK(g.laneCount(0) == 0);
}

void testTrackNotPersisted() {
  Fixture f;
  f.addTrack();
  f.persisted = false;                         // a tombstone, an aux child, or past the live count
  const std::string ev = callWrite(f, pointPayload(0, "cutoff"));
  CHECK(ev.find("track_not_persisted") != std::string::npos);
  // NO LANE CREATED. This is the whole point: the old behaviour accepted the write, reported
  // created_clip:true, and the points vanished at the next save with nothing having said no.
  CHECK(f.laneCount(0) == 0);

  // And it is refused for THAT reason, not for a missing track — the two are different bugs and a
  // handler that conflated them would send a caller looking in the wrong place.
  CHECK(ev.find("no_such_track") == std::string::npos);
}

// ----------------------------------------------------------------------------- what it does do
void testWritesAndAccumulates() {
  Fixture f;
  f.addTrack();
  callWrite(f, pointPayload(0, "cutoff", 0, 0.25f));
  CHECK(f.laneCount(0) == 1);
  CHECK(!f.trackTable.tracks[0]->track.automationClips.empty());
  if (f.trackTable.tracks[0]->track.automationClips.empty()) { return; }
  CHECK(f.trackTable.tracks[0]->track.automationClips[0].points().size() == 1);

  // A second point on the SAME param must extend that lane, not mint a second one carrying the
  // same id — two lanes with one paramId is a state no reader can disambiguate.
  callWrite(f, pointPayload(0, "cutoff", 480000, 0.75f));
  CHECK(f.laneCount(0) == 1);
  CHECK(f.trackTable.tracks[0]->track.automationClips[0].points().size() == 2);

  // A different param is a different lane.
  callWrite(f, pointPayload(0, "reso", 0, 0.1f));
  CHECK(f.laneCount(0) == 2);
}

void testTickCarriesAcrossTheSplit() {
  Fixture f;
  f.addTrack();
  // The tick is split across two 32-bit wire fields. A handler that read only the low word would
  // place every point past 2^32 nanoticks at the wrong time, and no short fixture would show it.
  const uint64_t big = (uint64_t{3} << 32) | 12345u;
  callWrite(f, pointPayload(0, "cutoff", big, 0.5f));
  CHECK(f.laneCount(0) == 1);
  // GUARDED, because indexing an empty vector is UB and a SEGFAULT reports nothing at all —
  // the run dies before any CHECK can print which assertion failed.
  CHECK(!f.trackTable.tracks[0]->track.automationClips.empty());
  if (f.trackTable.tracks[0]->track.automationClips.empty()) { return; }
  const auto& pts = f.trackTable.tracks[0]->track.automationClips[0].points();
  CHECK(pts.size() == 1);
  if (pts.size() == 1) CHECK(pts[0].nanotick == big);
}

}  // namespace

int main() {
  // Must be set before the first DAW_EVENT: event_log.cpp opens its sink from this env var into a
  // function-local static, once.
  static std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                            "/engine_automation_events.jsonl";
  std::remove(path.c_str());
  kLogPath = path.c_str();
  ::setenv("DAW_EVENT_LOG", kLogPath, 1);

  testEmptyParamId();
  testParamIdNotRepresentable();
  testNoSuchTrack();
  testTrackNotPersisted();
  testWritesAndAccumulates();
  testTickCarriesAcrossTheSplit();

  if (g_fail == 0) {
    std::printf("engine_automation_commands_tests: PASS\n");
    return 0;
  }
  std::printf("engine_automation_commands_tests: FAIL (%d)\n", g_fail);
  return 1;
}
