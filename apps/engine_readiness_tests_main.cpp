// Tests for the two readiness waits in apps/engine_audio_callback.h — awaitAnyReadyTrack and
// awaitAllReadyTracks.
//
// THIS FILE EXISTS BECAUSE THE RULE IT PINS HAD NO GUARD AT ALL. The offline pump must wait for
// EVERY unmuted track to be producing before it mixes block 1; waiting for ANY of them was task
// #16, where the first track to start producing let the render begin while another was still
// silent, and that track's note at tick 0 reached nothing and reappeared a whole loop later.
//
// The only detector was audio_loop, and it fails INTERMITTENTLY — 14 consecutive passes, then one
// failure, then 10 more passes. So reverting the fix left a green suite, which is the same shape as
// every other "green with the bug present" finding in this repository. The difference between the
// two functions is one observable and it can be stated exactly, so it should be.
//
// NO ENGINE, NO SHARED MEMORY, NO HOST. updateTracks() is the consumer thread's ordinary entry
// point and takes a plain vector, so the whole race is expressible as two atomics with the values
// they actually had when this was measured in a real render:
//
//     poll 0  track=0 muted=0 hostReady=1 active=0
//     poll 0  track=1 muted=0 hostReady=1 active=0
//
// Both hosts already up, neither track producing yet. That is the state the pump asks from.
#include "apps/engine_audio_callback.h"
#include "apps/engine_readiness_level.h"

#include <atomic>
#include <cstdio>
#include <vector>

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

// The atomics a TrackInfo points at. They must outlive the published list, which is why they are
// held here rather than constructed inline.
struct FakeTrack {
  std::atomic<uint32_t> completedBlockId{0};
  std::atomic<bool> hostReady{true};
  std::atomic<bool> active{false};
  std::atomic<bool> mute{false};
  std::atomic<bool> solo{false};
  daw::ShmHeader header{};
};

EngineAudioCallback::TrackInfo infoFor(FakeTrack& t, uint32_t trackId) {
  EngineAudioCallback::TrackInfo info;
  // completedBlockId and header must be non-null: awaitAnyReadyTrack skips a track without them
  // as "not a candidate", and a test where neither track is a candidate would be measuring that
  // rather than the readiness rule.
  info.completedBlockId = &t.completedBlockId;
  info.header = &t.header;
  info.hostReady = &t.hostReady;
  info.active = &t.active;
  info.mute = &t.mute;
  info.solo = &t.solo;
  info.trackId = trackId;
  return info;
}

// ---------------------------------------------------- the difference, stated exactly
void testAnyReturnsWhileATrackIsStillSilent() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.active.store(true);    // one track has started producing
  b.active.store(false);   // the other has not
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  // ANY IS SATISFIED. This is the defect, asserted as behaviour rather than described in a
  // comment: the render would start here, and everything track 1 owed to block 1 is missing.
  CHECK(cb.awaitAnyReadyTrack(50, /*requireActive=*/true));

  // ALL IS NOT, and it names the track that was behind.
  uint32_t late = 0xFFFFFFFFu;
  CHECK(!cb.awaitAllReadyTracks(50, /*requireActive=*/true, &late));
  CHECK(late == 1u);
}

// When every track is producing, ALL returns and returns promptly — a wait that never succeeds
// would be a deadlock dressed as safety, and the offline pump has no other way out.
void testAllReturnsWhenEveryTrackIsProducing() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.active.store(true);
  b.active.store(true);
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  uint32_t late = 0xFFFFFFFFu;
  CHECK(cb.awaitAllReadyTracks(2000, /*requireActive=*/true, &late));
}

// A HOST THAT IS NOT UP BLOCKS ALL TOO. hostReady and active are separate conditions and the
// measured failure was the second one — but a check that only covered `active` would let the first
// regress silently.
void testAllWaitsForTheHostAsWellAsProduction() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.active.store(true);
  b.active.store(true);
  b.hostReady.store(false);   // producing, but its host has not reported ready
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  uint32_t late = 0xFFFFFFFFu;
  CHECK(!cb.awaitAllReadyTracks(50, /*requireActive=*/true, &late));
  CHECK(late == 1u);
}

// A MUTED TRACK IS NOT WAITED FOR. It contributes nothing to the mix, and an all-muted project is
// a legitimate render of silence — waiting on one would deadlock a render that is correct. This is
// the same rule the any-variant already documents, and it has to hold for both or a muted track
// stalls every bounce.
void testMutedTracksAreNotWaitedFor() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.active.store(true);
  b.active.store(false);
  b.mute.store(true);          // silent by intent, not by lateness
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  uint32_t late = 0xFFFFFFFFu;
  CHECK(cb.awaitAllReadyTracks(2000, /*requireActive=*/true, &late));
}

// requireActive=false is the earlier question — "is the pipeline up" — and must not wait on
// production, or the pump could never reach the point where it starts the transport.
void testRequireActiveFalseIgnoresProduction() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.active.store(false);
  b.active.store(false);       // nothing is producing yet, which is expected before arming
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  uint32_t late = 0xFFFFFFFFu;
  CHECK(cb.awaitAllReadyTracks(2000, /*requireActive=*/false, &late));
}

// ------------------------------------------------------ A SOLOED-OUT TRACK IS NOT WAITED FOR
// THE RULE HAD TWO ANSWERS. process()'s summing loop resolved mute AND solo; the priming loop and
// all three waits tested mute only. So with any track soloed, this wait blocked on tracks the mix
// was about to discard — for the full timeout, and then FAILED THE RENDER with "track N was still
// not producing", naming a track whose audio was never going to be used.
//
// Both of this file's previous subjects were the same shape: two copies of "which tracks count"
// disagreeing. That is why the mute/solo question is now one function, asserted here.
void testSoloedOutTrackIsNotWaitedFor() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.active.store(true);
  a.solo.store(true);     // the one the user wants to hear
  b.active.store(false);  // not soloed, never produces — the mix will discard it anyway
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  uint32_t late = 0xFFFFFFFFu;
  CHECK(cb.awaitAllReadyTracks(2000, /*requireActive=*/true, &late));
}

// AND SOLO DOES NOT EXCUSE THE TRACK THAT IS ACTUALLY SOLOED. The rule must still wait for what
// the mix WILL read, or this fix would trade a stalled render for a truncated one.
void testTheSoloedTrackIsStillWaitedFor() {
  std::atomic<uint32_t> playbackBlockId{0};
  EngineAudioCallback cb(44100.0, 512, 3, &playbackBlockId);

  FakeTrack a, b;
  a.solo.store(true);
  a.active.store(false);  // soloed AND late: this one must block
  b.active.store(true);
  cb.updateTracks({infoFor(a, 0), infoFor(b, 1)});

  uint32_t late = 0xFFFFFFFFu;
  CHECK(!cb.awaitAllReadyTracks(50, /*requireActive=*/true, &late));
  CHECK(late == 0u);
}


// ─────────────────────────────────────────────────────────────────────────────────────────────
// TWO-LEVEL READINESS (AE-P1.2 G2-B / R2). apps/engine_readiness_level.h names an ordering that
// hostReady and mirrorPending already encoded; these pin the three transitions that matter and the
// wedge that has no observable symptom.
//
// The subject is the pure predicate, not TrackRuntime: a level that can only be observed by
// standing up a host is a level nobody can test, and the flags are what the producer actually reads.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// COLD START. Not mapped, so nothing may be dispatched.
void testColdStartIsNotMapped() {
  CHECK(daw::readinessLevel(/*hostReady=*/false) == daw::HostReadiness::NotMapped);
  CHECK(!daw::readinessAtLeast(daw::readinessLevel(false),
                               daw::HostReadiness::MappedAndDispatchable));
}

// MAPPED MEANS DISPATCHABLE, and that is all it means. It does NOT mean bypassed: applyHostBypassStates
// goes through sendSetBypass, which is fire-and-forget (host_controller.cpp:595-601 returns whether
// the SEND succeeded), so the engine never learns a plugin was bypassed.
void testMappedMeansDispatchableAndNothingMore() {
  CHECK(daw::readinessLevel(/*hostReady=*/true) == daw::HostReadiness::MappedAndDispatchable);
  CHECK(daw::readinessAtLeast(daw::readinessLevel(true),
                              daw::HostReadiness::MappedAndDispatchable));
}

// THE MIRROR IS A SEPARATE QUESTION, NOT A HIGHER LEVEL. It can become outstanding again at any
// time — engine_render_track.cpp:554 arms a replay on note-ring overflow, mid-render — so folding
// it into an ordered level would imply a completion that does not exist.
void testMirrorIsOrthogonalToTheLevel() {
  CHECK(daw::mirrorOutstanding(true));
  CHECK(!daw::mirrorOutstanding(false));
  // the level is the same either way: mapped is mapped
  CHECK(daw::readinessLevel(true) == daw::HostReadiness::MappedAndDispatchable);
}

// A REPLAY ARMED ON A DOWN HOST CAN NEVER COMPLETE. engine_producer_thread.cpp:195 skips a runtime
// that is not hostReady before it can clear mirrorPending, so the producer sits in mirrorOnly
// emitting nothing with nothing saying why — the aux-child wedge engine_rt_helpers.h:59-62 names.
void testAnArmedMirrorOnADownHostCanNeverComplete() {
  CHECK(!daw::mirrorReplayCanComplete(/*hostReady=*/false, /*mirrorPending=*/true));
  CHECK(daw::mirrorReplayCanComplete(true, true));
  CHECK(daw::mirrorReplayCanComplete(true, false));
  CHECK(daw::mirrorReplayCanComplete(false, false));
}

// NEGATIVE CONTROL, SWAPPED ORDER. If the comparison were not strict, a site asking for the higher
// level would be satisfied by the lower — the failure this ordering exists to prevent.
void testTheOrderIsStrictInBothDirections() {
  const auto none = daw::HostReadiness::NotMapped;
  const auto mapped = daw::HostReadiness::MappedAndDispatchable;
  CHECK(daw::readinessAtLeast(mapped, none));
  CHECK(daw::readinessAtLeast(mapped, mapped));
  CHECK(!daw::readinessAtLeast(none, mapped));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// HOST GENERATION (P2-HOST-02a). Constructed and carried; no reader consults it yet. These pin the
// two properties a later reader will depend on, so 02b cannot be built on an assumption.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// MONOTONIC WITHIN A LIFETIME. A generation that can repeat cannot distinguish mappings, which is
// the entire purpose.
void testGenerationIsStrictlyIncreasing() {
  uint32_t g = 0;
  for (int i = 0; i < 1000; ++i) {
    const uint32_t next = daw::nextHostGeneration(g);
    CHECK(next != g);
    CHECK(next == g + 1u);
    g = next;
  }
  CHECK(g == 1000u);
}

// ZERO MEANS NEVER LAUNCHED, SO THE WRAP MUST SKIP IT. A counter wrapping to 0 would report a host
// on its 4,294,967,296th launch as one that has never started — and the 02b reader would read that
// as "no mapping to compare", which is the safe-LOOKING answer and the wrong one.
void testGenerationWrapSkipsZero() {
  CHECK(daw::nextHostGeneration(0xFFFFFFFEu) == 0xFFFFFFFFu);
  CHECK(daw::nextHostGeneration(0xFFFFFFFFu) == 1u);   // not 0
  CHECK(daw::nextHostGeneration(0xFFFFFFFFu) != 0u);
  // and the predicate agrees on both sides of the wrap
  CHECK(daw::hostEverLaunched(daw::nextHostGeneration(0xFFFFFFFFu)));
  CHECK(!daw::hostEverLaunched(0u));
  CHECK(daw::hostEverLaunched(1u));
}

// THE INITIAL STATE IS DISTINGUISHABLE. A fresh runtime has never launched, and the first launch
// must leave it saying so.
void testFirstLaunchLeavesNeverLaunchedBehind() {
  const uint32_t fresh = 0;
  CHECK(!daw::hostEverLaunched(fresh));
  CHECK(daw::hostEverLaunched(daw::nextHostGeneration(fresh)));
}

}  // namespace

int main() {
  testGenerationIsStrictlyIncreasing();
  testGenerationWrapSkipsZero();
  testFirstLaunchLeavesNeverLaunchedBehind();
  testColdStartIsNotMapped();
  testMappedMeansDispatchableAndNothingMore();
  testMirrorIsOrthogonalToTheLevel();
  testAnArmedMirrorOnADownHostCanNeverComplete();
  testTheOrderIsStrictInBothDirections();
  testSoloedOutTrackIsNotWaitedFor();
  testTheSoloedTrackIsStillWaitedFor();
  testAnyReturnsWhileATrackIsStillSilent();
  testAllReturnsWhenEveryTrackIsProducing();
  testAllWaitsForTheHostAsWellAsProduction();
  testMutedTracksAreNotWaitedFor();
  testRequireActiveFalseIgnoresProduction();

  if (g_fail != 0) {
    std::printf("engine_readiness_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_readiness_tests: PASS\n");
  return 0;
}
