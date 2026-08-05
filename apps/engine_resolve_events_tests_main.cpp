// Tests for apps/engine_resolve_events.h — the first piece of renderTrack that can be asked a
// question without booting a process and rendering audio.
//
// THIS FILE IS THE POINT OF THE EXTRACTION. renderTrack is 1,600 lines and every rule inside it
// was reachable only through a full engine: build a project, launch a host, render, and infer the
// rule from a waveform. A maintainability panel graded the structure C — "relocation is not
// decomposition" — and separately noted that 99% of the suite's runtime boots processes. Both
// complaints have the same cause and the same cure, and this is what the cure looks like: a rule
// that used to need a render now needs a vector.
//
// THE FIXTURE IS THE COST, and it is paid once. RenderTrackDeps has eighteen members, so the
// lines below are what it takes to ask this function anything at all. The next two extractions out
// of renderTrack — runNode and emitNotes — reuse it unchanged.
#include "apps/engine_resolve_events.h"

#include "apps/engine_rt_helpers.h"

#include <cstdio>
#include <cstring>

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

// Everything RenderTrackDeps refers to, owned here so the deps struct's references stay valid.
struct Fixture {
  daw::HostConfig engineConfig{};
  std::atomic<uint64_t> lastOverflowTick{0};
  std::atomic<uint32_t> nextNoteId{1};
  std::atomic<bool> warnedEventOutsideBlock{false};

  TrackRuntime runtime;
  TrackStateSnapshot trackState{};
  std::vector<daw::EventEntry> scratchpad;

  // THE FIXTURE SHRANK WITH THE SIGNATURE. It used to build a whole eighteen-member
  // RenderTrackDeps — a TempoMapProvider, a WorkerPool pointer, a PatcherGraphOwner, a
  // TransportState — none of which this function touches. It now needs the four things it
  // actually reads. That is the test-side evidence that the narrowing was real coupling removed
  // and not bookkeeping: a fixture cannot be smaller than the thing it constructs.
  //
  // getHarmonyAt AND getScaleForHarmony MUST RESOLVE. The first draft returned nullopt/nullptr
  // for them, and every MusicalLogic entry then hit `if (!harmony) continue;` and was dropped
  // before it reached anything worth asserting — so the gate test below passed with the gate
  // check DELETED, and the degree test could not have been written at all. Its negative control
  // is what caught it. A fixture that cannot reach the code is not a cheap fixture, it is a green
  // test of nothing.
  NoteResolution noteResolution{
      [](uint64_t) -> std::optional<daw::HarmonyEvent> {
        return daw::HarmonyEvent{0, 0, 1, 0};
      },
      [](const daw::HarmonyEvent& h) -> const daw::Scale* {
        return daw::ScaleRegistry::instance().find(h.scaleId);
      },
      [](uint8_t pitch, const daw::HarmonyEvent&) -> daw::ResolvedPitch {
        return daw::ResolvedPitch{pitch, 0.0f, static_cast<double>(pitch) * 100.0};
      },
      [](uint64_t tick) -> uint64_t { return tick; },
      nextNoteId};

  Fixture() {
    engineConfig.blockSize = 512;
    engineConfig.sampleRate = 44100.0;
    scratchpad.resize(64);
  }

  uint32_t run(uint32_t count) {
    return resolveMusicalLogicAndSort(noteResolution, engineConfig, lastOverflowTick,
                                      warnedEventOutsideBlock, runtime, trackState, scratchpad,
                                      count,
                                      /*blockSampleStart=*/0, /*windowStartTicks=*/0,
                                      /*windowEndTicks=*/1000000,
                                      /*samplesPerTick=*/1.0L, /*midiChannel=*/0);
  }
};

daw::EventEntry midiAt(uint64_t sampleTime, uint8_t pitch) {
  daw::EventEntry e{};
  e.type = static_cast<uint16_t>(daw::EventType::Midi);
  e.sampleTime = sampleTime;
  daw::MidiPayload m{};
  m.status = 0x90;
  m.data1 = pitch;
  m.data2 = 100;
  e.size = sizeof(m);
  std::memcpy(e.payload, &m, sizeof(m));
  return e;
}

// ------------------------------------------------------- everything that is not logic survives
// A pass-through that dropped or reordered ordinary MIDI would be catastrophic and invisible in a
// render full of other notes. It is one assertion here.
void testNonLogicEventsPassThroughUnchanged() {
  Fixture f;
  f.scratchpad[0] = midiAt(100, 60);
  f.scratchpad[1] = midiAt(200, 62);
  f.scratchpad[2] = midiAt(300, 64);
  const uint32_t out = f.run(3);
  CHECK(out == 3);
  if (out == 3) {
    daw::MidiPayload m{};
    std::memcpy(&m, f.scratchpad[0].payload, sizeof(m));
    CHECK(f.scratchpad[0].sampleTime == 100);
    CHECK(m.data1 == 60);
    std::memcpy(&m, f.scratchpad[2].payload, sizeof(m));
    CHECK(f.scratchpad[2].sampleTime == 300);
    CHECK(m.data1 == 64);
  }
}

// ------------------------------------------------------------------ the block comes out ORDERED
// THE HOST DEPENDS ON THIS. juce_host_process_main windows a block's events and converts each to
// an offset; it does not re-sort. An out-of-order scratchpad is out-of-order MIDI, which a plugin
// is entitled to handle any way it likes — including badly.
void testOutputIsSortedBySampleTime() {
  Fixture f;
  f.scratchpad[0] = midiAt(300, 64);
  f.scratchpad[1] = midiAt(100, 60);
  f.scratchpad[2] = midiAt(200, 62);
  const uint32_t out = f.run(3);
  CHECK(out == 3);
  if (out == 3) {
    CHECK(f.scratchpad[0].sampleTime == 100);
    CHECK(f.scratchpad[1].sampleTime == 200);
    CHECK(f.scratchpad[2].sampleTime == 300);
  }
}

// AT THE SAME SAMPLE, PRIORITY DECIDES — the tiebreak is not incidental. A param change and a
// note-on landing on one sample must not arrive in whichever order the producer happened to append
// them, or a note plays with the previous block's parameter value. daw::engine::priorityForEvent
// owns the ordering; this asserts the sort actually consults it.
void testEqualSampleTimesOrderByPriority() {
  Fixture f;
  daw::EventEntry note = midiAt(500, 60);
  daw::EventEntry param{};
  param.type = static_cast<uint16_t>(daw::EventType::Param);
  param.sampleTime = 500;
  daw::ParamPayload p{};
  p.value = 0.5f;
  param.size = sizeof(p);
  std::memcpy(param.payload, &p, sizeof(p));

  f.scratchpad[0] = note;   // appended in the WRONG order on purpose
  f.scratchpad[1] = param;
  const uint32_t out = f.run(2);
  CHECK(out == 2);
  if (out == 2) {
    const auto first = priorityForEvent(f.scratchpad[0]);
    const auto second = priorityForEvent(f.scratchpad[1]);
    CHECK(first <= second);
    // And concretely: the parameter must be the one that comes first at an equal timestamp.
    CHECK(f.scratchpad[0].type == static_cast<uint16_t>(daw::EventType::Param));
  }
}

// ------------------------------------------------------------------ OVERFLOW DROPS, IT DOES NOT
// ------------------------------------------------------------------ RUN OFF THE END OF THE BUFFER
//
// THE HEADER PROMISED THIS AND THE CODE HONOURED IT AT ONE OF THREE WRITE SITES. outCount is not
// bounded by the input count: a MusicalLogic entry carrying a duration emits a note-ON and a
// note-OFF, so N of them produce up to 2N. The note-off went through the guarded appender and was
// dropped correctly; the two unguarded writes then indexed past the end of the vector.
//
// AND THE ONLY OVERFLOW ASSERTION IN THIS FILE USED TO BE `lastOverflowTick == 0` ON EMPTY INPUT,
// which is true of a function that does nothing at all. A contract stated in a header and tested
// with a tautology is worth less than no contract, because it stops anyone looking.
void testOverflowIsReportedAndBounded() {
  Fixture f;
  // A SMALL SCRATCHPAD so the boundary is reachable in a test rather than only at 1024 events.
  // The engine sizes this to kPatcherScratchpadCapacity; the rule is about the relationship
  // between outCount and size(), not about the specific number.
  f.scratchpad.assign(4, daw::EventEntry{});
  const uint32_t kInput = 6;
  for (uint32_t i = 0; i < kInput; ++i) {
    daw::EventEntry degree{};
    degree.type = static_cast<uint16_t>(daw::EventType::MusicalLogic);
    degree.sampleTime = 10 * i;
    daw::MusicalLogicPayload logic{};
    logic.metadata[0] = daw::kMusicalLogicKindDegree;
    logic.degree = 0;
    logic.base_octave = 5;
    logic.velocity = 100;
    logic.duration_ticks = 100;   // makes each entry emit a note-off as well as a note-on
    degree.size = sizeof(logic);
    std::memcpy(degree.payload, &logic, sizeof(logic));
    f.scratchpad[i] = degree;
  }

  const uint32_t out = f.run(kInput);

  // NEVER PAST THE END. Six inputs each wanting two events is twelve into a buffer of eight.
  CHECK(out <= f.scratchpad.size());
  // AND IT SAID SO. The tick of the first lost event is published rather than the loss being
  // silent, which is the whole difference between a documented limit and a corruption.
  // THE TICK OF THE LAST EVENT LOST — 50, the sixth entry's, MEASURED. Not "nonzero": that would
  // pass on a function that published any tick at all, including the wrong one. The store
  // overwrites on each drop, so the survivor is the most recent, and the header says so now.
  CHECK(f.lastOverflowTick.load() == 50);
}

// -------------------------------------------------------------------- an empty block is not work
// scratchpadCount == 0 must come back 0 rather than reading scratchpad[0], which the caller has
// resized but never written. The caller guards this with `eventDirty`, so the guard and the
// function have to agree about who is responsible.
void testEmptyInputProducesEmptyOutput() {
  Fixture f;
  CHECK(f.run(0) == 0);
  CHECK(f.lastOverflowTick.load() == 0);
}

// ------------------------------------------------------------ a gate is a decision, not an event
// kMusicalLogicKindGate entries are consumed here and must not reach the host: a gate says whether
// something plays, and forwarding it as an event would put an uninterpretable payload on the wire.
void testGateLogicIsConsumedNotForwarded() {
  Fixture f;
  daw::EventEntry gate{};
  gate.type = static_cast<uint16_t>(daw::EventType::MusicalLogic);
  gate.sampleTime = 100;
  daw::MusicalLogicPayload logic{};
  logic.metadata[0] = daw::kMusicalLogicKindGate;
  gate.size = sizeof(logic);
  std::memcpy(gate.payload, &logic, sizeof(logic));

  f.scratchpad[0] = gate;
  f.scratchpad[1] = midiAt(200, 62);
  const uint32_t out = f.run(2);
  // The gate is gone; the ordinary note is not.
  CHECK(out == 1);
  if (out == 1) {
    CHECK(f.scratchpad[0].type == static_cast<uint16_t>(daw::EventType::Midi));
    CHECK(f.scratchpad[0].sampleTime == 200);
  }
}

// ------------------------------------------------------------- and a degree becomes a real note
// THE FUNCTION'S ACTUAL JOB, and it was unreachable until the fixture above learned to answer the
// harmony lookup. A MusicalLogic entry naming a scale DEGREE is rewritten in place into MIDI: same
// slot, new type, a pitch resolved through the scale and a note id allocated from the engine's
// counter. If this regressed, every generative track would fall silent while every hand-written
// note kept playing — which is a bug report nobody would think to file against a sort function.
void testDegreeLogicBecomesMidi() {
  Fixture f;
  daw::EventEntry degree{};
  degree.type = static_cast<uint16_t>(daw::EventType::MusicalLogic);
  degree.sampleTime = 100;
  daw::MusicalLogicPayload logic{};
  logic.metadata[0] = daw::kMusicalLogicKindDegree;
  logic.degree = 0;          // the root of the scale
  logic.base_octave = 5;
  logic.octave_offset = 0;
  logic.velocity = 100;
  degree.size = sizeof(logic);
  std::memcpy(degree.payload, &logic, sizeof(logic));

  f.scratchpad[0] = degree;
  const uint32_t before = f.nextNoteId.load();
  const uint32_t out = f.run(1);

  CHECK(out == 1);
  if (out == 1) {
    CHECK(f.scratchpad[0].type == static_cast<uint16_t>(daw::EventType::Midi));
    CHECK(f.scratchpad[0].sampleTime == 100);   // the position it was generated for survives
    daw::MidiPayload m{};
    std::memcpy(&m, f.scratchpad[0].payload, sizeof(m));
    CHECK((m.status & 0xF0) == 0x90);           // a note-ON
    CHECK(m.data2 == 100);                      // the velocity it asked for
    // Degree 0 of scale 1 rooted at C with base octave 5 is MIDI 72 — MEASURED, after 60 was
    // guessed and was wrong. A concrete pitch rather than "some pitch", because an assertion that
    // only says "nonzero" passes on a resolver that always answers the same note.
    CHECK(m.data1 == 72);
  }
  // A NOTE ID WAS ALLOCATED. Note-offs are matched by id, so a resolved note that never took one
  // is a note that can never be stopped.
  CHECK(f.nextNoteId.load() > before);
}

}  // namespace

int main() {
  testDegreeLogicBecomesMidi();
  testOverflowIsReportedAndBounded();
  testNonLogicEventsPassThroughUnchanged();
  testOutputIsSortedBySampleTime();
  testEqualSampleTimesOrderByPriority();
  testEmptyInputProducesEmptyOutput();
  testGateLogicIsConsumedNotForwarded();

  if (g_fail != 0) {
    std::printf("engine_resolve_events_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_resolve_events_tests: PASS\n");
  return 0;
}
