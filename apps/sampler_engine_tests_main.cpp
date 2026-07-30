// THE POLYPHONIC LAYER: choke, NNA, stealing, and sample-accurate note-OFF.
//
// Note-ON at an offset is easy and would work without segmenting the block. Note-OFF at an offset
// is not expressible any other way, and getting it wrong makes a short gated note a different
// length depending on the buffer size — which is a determinism failure that sounds like nothing at
// all until you bounce.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "apps/sampler_engine.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
}

void checkNear(float got, float want, float tol, const char* what) {
  if (!(std::fabs(got - want) <= tol)) {
    std::printf("FAIL %s: got %.6f want %.6f (tol %.6f)\n", what, got, want, tol);
    ++g_fail;
  }
}

// DC at 1.0, so the output IS the gain structure and nothing else — an oscillating fixture would
// make "is this voice sounding" a question about phase.
using daw::SamplerEvent;
using daw::SamplerEventKind;

std::shared_ptr<daw::SamplerSourceAudio> dcSource(uint64_t frames, double rate = 48000.0) {
  auto a = std::make_shared<daw::SamplerSourceAudio>();
  a->channels.assign(1, std::vector<float>(frames, 1.0f));
  a->frames = frames;
  a->sampleRate = rate;
  a->buildPlanes();
  return a;
}

// One kit, N slots on consecutive keys from 36, all sharing mod set 1.
std::shared_ptr<daw::SamplerRender> makeKit(uint32_t slots,
                                            bool gated = false,
                                            uint64_t frames = 480000) {
  auto r = std::make_shared<daw::SamplerRender>();
  r->sampleRate = 48000.0;
  r->state.sources.push_back({1, "dc.wav", 0, 0});
  r->audio.push_back(dcSource(frames));
  for (uint32_t i = 0; i < slots; ++i) {
    daw::SamplerSlot s;
    s.id = static_cast<uint16_t>(i + 1);
    s.sourceLocalId = 1;
    s.keyLow = s.keyHigh = s.rootKey = static_cast<uint8_t>(36 + i);
    s.gate = gated ? 1 : 0;
    s.panThousandths = -1000;  // hard left, so the pan law is unity and gains read directly
    s.modSetId = 1;
    r->state.slots.push_back(s);
  }
  // No amp envelope: this file is about voice management, and an envelope would put a decay
  // between every assertion and the thing it is asserting.
  daw::SamplerModSet m;
  m.id = 1;
  r->state.modSets.push_back(m);
  r->keymap.rebuild(r->state);
  return r;
}

struct Out {
  std::vector<float> l, r;
  float* planes[2];
  explicit Out(uint32_t n) : l(n, 0.0f), r(n, 0.0f) {
    planes[0] = l.data();
    planes[1] = r.data();
  }
};

SamplerEvent on(uint32_t off, uint8_t pitch, uint32_t noteId, uint8_t column = 0,
                uint16_t sound = 0, uint8_t vel = 127) {
  SamplerEvent e;
  e.offsetInBlock = off;
  e.kind = SamplerEventKind::NoteOn;
  e.pitch = pitch;
  e.velocity = vel;
  e.column = column;
  e.sound = sound;
  e.noteId = noteId;
  return e;
}

SamplerEvent off(uint32_t offset, uint32_t noteId) {
  SamplerEvent e;
  e.offsetInBlock = offset;
  e.kind = SamplerEventKind::NoteOff;
  e.noteId = noteId;
  return e;
}

}  // namespace

int main() {
  // ---- A NOTE SOUNDS, AT ITS OWN SAMPLE. Silence before, signal after.
  {
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(512);
    SamplerEvent ev[] = {on(100, 36, 1)};
    rt.render(o.planes, 2, 512, ev, 1);
    checkNear(o.l[99], 0.0f, 1e-6f, "silence up to the note's own sample");
    checkNear(o.l[100], 1.0f, 1e-3f, "and full level from it");
    check(rt.activeVoices() == 1, "one voice is sounding");
  }

  // ---- SAMPLE-ACCURATE NOTE-OFF. This is what block segmentation is FOR. A gated note held for
  // 300 samples must be 300 samples long, whatever the block size.
  {
    auto kit = makeKit(4, /*gated=*/true);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(512);
    SamplerEvent ev[] = {on(0, 36, 1), off(300, 1)};
    rt.render(o.planes, 2, 512, ev, 2);
    checkNear(o.l[299], 1.0f, 1e-3f, "sounding right up to the note-off's sample");
    checkNear(o.l[300], 0.0f, 1e-6f,
              "and silent from it — with no envelope, note-off ends the voice AT its own sample. "
              "A note-off quantised to the block boundary would make this note 512 samples long "
              "at one buffer size and 300 at another");
    check(rt.activeVoices() == 0, "and the voice is freed");
  }

  // ---- A ONE-SHOT IGNORES NOTE-OFF. The difference between a drum and a pad, decided by the
  // slot rather than by whether the tracker authored an OFF.
  {
    auto kit = makeKit(4, /*gated=*/false);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(512);
    SamplerEvent ev[] = {on(0, 36, 1), off(200, 1)};
    rt.render(o.planes, 2, 512, ev, 2);
    checkNear(o.l[400], 1.0f, 1e-3f, "a ONE-SHOT keeps sounding through its own note-off");
    check(rt.activeVoices() == 1, "and keeps its voice");
  }

  // ---- POLYPHONY. Three notes at three offsets, and the level steps up at each one — a running
  // sum, which is also the check that voices are not overwriting each other's output.
  {
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(512);
    SamplerEvent ev[] = {on(0, 36, 1), on(100, 37, 2), on(200, 38, 3)};
    rt.render(o.planes, 2, 512, ev, 3);
    checkNear(o.l[50], 1.0f, 1e-3f, "one voice");
    checkNear(o.l[150], 2.0f, 1e-3f, "two voices SUM rather than replace");
    checkNear(o.l[250], 3.0f, 1e-3f, "three");
    check(rt.activeVoices() == 3, "all three are alive");
  }

  // ---- SEVERAL EVENTS ON ONE SAMPLE all take effect before any audio is produced at it. A
  // segmenting loop that renders a zero-length span between them would spin or drop one.
  {
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(256);
    SamplerEvent ev[] = {on(64, 36, 1), on(64, 37, 2), on(64, 38, 3)};
    rt.render(o.planes, 2, 256, ev, 3);
    checkNear(o.l[64], 3.0f, 1e-3f, "three note-ons at the SAME sample all sound from it");
    check(rt.activeVoices() == 3, "and all three allocated");
  }

  // ---- VOICE GROUP CHOKE: open hat / closed hat. Two fields, no container.
  {
    auto kit = makeKit(4);
    kit->state.slots[0].voiceGroup = 1;  // key 36
    kit->state.slots[1].voiceGroup = 1;  // key 37 — same group
    kit->state.slots[2].voiceGroup = 0;  // key 38 — unrelated
    kit->state.slots[0].chokeFadeUs = 1000;  // 1 ms, so the fade is measurable in this block
    kit->state.slots[1].chokeFadeUs = 1000;
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(2048);
    SamplerEvent ev[] = {on(0, 36, 1), on(1000, 37, 2)};
    rt.render(o.planes, 2, 2048, ev, 2);
    checkNear(o.l[500], 1.0f, 1e-3f, "the open hat is sounding alone");
    // After the choke fade (48 frames at 1 ms/48k) only the new voice remains.
    checkNear(o.l[1500], 1.0f, 2e-2f,
              "and after the closed hat chokes it, ONE voice remains rather than two summing");
    check(rt.activeVoices() == 1, "the choked voice is gone");
    // ...and the choke is a RAMP. An instant cut here is the classic hi-hat click.
    float maxJump = 0.0f;
    for (uint32_t i = 1001; i < 1100; ++i) {
      maxJump = std::max(maxJump, std::fabs(o.l[i] - o.l[i - 1]));
    }
    check(maxJump < 0.5f, "the choke RAMPS rather than cutting — an instant stop mid-waveform is "
                          "the classic hi-hat click");
  }
  {
    // NEGATIVE CONTROL for choke: different groups must NOT choke, or every kit is monophonic.
    auto kit = makeKit(4);
    kit->state.slots[0].voiceGroup = 1;
    kit->state.slots[1].voiceGroup = 2;
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(2048);
    SamplerEvent ev[] = {on(0, 36, 1), on(1000, 37, 2)};
    rt.render(o.planes, 2, 2048, ev, 2);
    check(rt.activeVoices() == 2, "DIFFERENT voice groups do not choke each other");
    checkNear(o.l[1500], 2.0f, 1e-3f, "and both are audible");
  }
  {
    // ...and group ZERO is not a group. Two un-grouped slots must not choke, which they would if
    // the comparison forgot to exclude 0 — the sentinel-collides-with-a-legal-value bug again.
    auto kit = makeKit(4);  // all voiceGroup == 0
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(2048);
    SamplerEvent ev[] = {on(0, 36, 1), on(1000, 37, 2)};
    rt.render(o.planes, 2, 2048, ev, 2);
    check(rt.activeVoices() == 2,
          "voiceGroup 0 means NO GROUP — if 0 choked 0, a whole kit would be monophonic");
  }

  // ---- NNA. The three behaviours against a repeated note in one column.
  {
    // Cut (the default): the previous voice stops.
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(4096);
    SamplerEvent ev[] = {on(0, 36, 1), on(2000, 36, 2)};
    rt.render(o.planes, 2, 4096, ev, 2);
    checkNear(o.l[3000], 1.0f, 2e-2f, "NNA=Cut leaves ONE voice after a repeat");
  }
  {
    // Continue: the previous voice keeps ringing. THIS is the one the tracker survey flagged as
    // currently losing work, and it needs no code at all — only the decision not to destroy.
    auto kit = makeKit(4);
    kit->state.slots[0].nna = daw::SamplerNna::Continue;
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(4096);
    SamplerEvent ev[] = {on(0, 36, 1), on(2000, 36, 2)};
    rt.render(o.planes, 2, 4096, ev, 2);
    checkNear(o.l[3000], 2.0f, 1e-3f,
              "NNA=Continue lets the previous note RING — an arpeggiated chord down one column, "
              "which is the gesture a truncate-on-entry model cannot express at all");
    check(rt.activeVoices() == 2, "both voices alive");
  }
  {
    // NoteOff: the previous voice releases rather than cutting. With no envelope that ends it,
    // which is still distinguishable from Continue.
    auto kit = makeKit(4, /*gated=*/true);
    kit->state.slots[0].nna = daw::SamplerNna::NoteOff;
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(4096);
    SamplerEvent ev[] = {on(0, 36, 1), on(2000, 36, 2)};
    rt.render(o.planes, 2, 4096, ev, 2);
    check(rt.activeVoices() == 1, "NNA=NoteOff releases the previous voice");
  }
  {
    // NNA is scoped to the COLUMN. The same slot in two columns is two independent lanes, which
    // is what makes a tracker's columns mean anything.
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(4096);
    SamplerEvent ev[] = {on(0, 36, 1, /*column=*/0), on(2000, 36, 2, /*column=*/1)};
    rt.render(o.planes, 2, 4096, ev, 2);
    check(rt.activeVoices() == 2,
          "the same sound in a DIFFERENT column does not cut — columns are independent lanes");
  }

  // ---- STEALING. A pool of 4 hit with 6 notes must not drop notes silently, must not exceed its
  // cap in a way that grows unboundedly, and must REPORT that it stole.
  {
    auto kit = makeKit(8);
    daw::SamplerRuntime rt;
    rt.configure(4, 48000.0);
    rt.setSnapshot(kit);
    Out o(4096);
    SamplerEvent ev[16];
    for (uint32_t i = 0; i < 16; ++i) {
      ev[i] = on(i * 100, static_cast<uint8_t>(36 + (i % 8)), i + 1, static_cast<uint8_t>(i % 8));
    }
    rt.render(o.planes, 2, 4096, ev, 16);
    check(rt.stealCount() > 0,
          "a pool of 4 hit with 16 notes STEALS, and says so — a pool running out is a musical "
          "fact the user should be told, not a silent truncation");
    check(rt.activeVoices() <= 4,
          "and the pool is EXACTLY its cap — a voice cap that is not the voice count cannot be "
          "reasoned about, and silently raises the CPU ceiling the setting exists to bound");
    // The last note must be audible: stealing exists so the NEWEST note always plays.
    check(o.l[1600] > 0.5f, "and the most recent notes are the ones sounding");
  }

  // ---- AN UNMAPPED KEY IS SILENT AND COUNTED. Not a crash, not the nearest drum.
  {
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(256);
    SamplerEvent ev[] = {on(0, 100, 1)};  // nothing mapped at 100
    rt.render(o.planes, 2, 256, ev, 1);
    checkNear(o.l[128], 0.0f, 1e-6f, "an unmapped key is SILENT");
    check(rt.activeVoices() == 0, "and allocates no voice");
    check(rt.unmappedCount() == 1, "and is counted, so a kit that is silent everywhere is "
                                   "diagnosable without a debugger");
  }

  // ---- THE SOUND ADDRESS (R2). `sound != 0` names the slot directly, and PITCH still means
  // varispeed — which is the whole amen-break gesture: one snare, five pitches, one column.
  {
    auto kit = makeKit(4);
    daw::SamplerRuntime rt;
    rt.configure(16, 48000.0);
    rt.setSnapshot(kit);
    Out o(256);
    // Slot 2 is mapped to key 37, but we address it by id while playing key 60.
    SamplerEvent ev[] = {on(0, 60, 1, 0, /*sound=*/2)};
    rt.render(o.planes, 2, 256, ev, 1);
    check(rt.activeVoices() == 1,
          "an explicit `sound` plays that slot from a key it is not mapped to — the amen gesture");
    check(rt.unmappedCount() == 0, "and does not go through the keymap at all");
  }

  // ---- THE 9xx SEEK, as a FRACTION of the slot's extent. Absolute frames would break when the
  // slot's sample is swapped, and here a slot can name a slice, so it would break on a re-chop.
  {
    auto kit = makeKit(1, false, /*frames=*/1000);
    // A ramp rather than DC, so the read POSITION is observable.
    auto a = std::make_shared<daw::SamplerSourceAudio>();
    a->channels.assign(1, std::vector<float>(1000));
    for (uint64_t i = 0; i < 1000; ++i) {
      a->channels[0][i] = static_cast<float>(i) / 1000.0f;
    }
    a->frames = 1000;
    a->sampleRate = 48000.0;
    a->buildPlanes();
    kit->audio[0] = a;

    daw::SamplerRuntime rt;
    rt.configure(4, 48000.0);
    rt.setSnapshot(kit);
    Out o(64);
    SamplerEvent e = on(0, 36, 1);
    e.offsetFrac = 32768;  // half way in
    SamplerEvent ev[] = {e};
    rt.render(o.planes, 2, 64, ev, 1);
    checkNear(o.l[0], 0.5f, 2e-2f, "a 50% offset starts playback half way into the slot");
  }
  {
    // NEGATIVE CONTROL: no offset starts at the beginning, or the fraction could be ignored and
    // the test above would still pass on a sample that happens to start at 0.5.
    auto kit = makeKit(1, false, 1000);
    auto a = std::make_shared<daw::SamplerSourceAudio>();
    a->channels.assign(1, std::vector<float>(1000));
    for (uint64_t i = 0; i < 1000; ++i) {
      a->channels[0][i] = static_cast<float>(i) / 1000.0f;
    }
    a->frames = 1000;
    a->sampleRate = 48000.0;
    a->buildPlanes();
    kit->audio[0] = a;
    daw::SamplerRuntime rt;
    rt.configure(4, 48000.0);
    rt.setSnapshot(kit);
    Out o(64);
    SamplerEvent ev[] = {on(0, 36, 1)};
    rt.render(o.planes, 2, 64, ev, 1);
    checkNear(o.l[0], 0.0f, 1e-3f, "and no offset starts at the beginning");
  }

  // ---- VARISPEED THROUGH THE SLOT'S ROOT KEY. An octave up is exactly twice the rate.
  {
    auto kit = makeKit(1, false, 4000);
    auto a = std::make_shared<daw::SamplerSourceAudio>();
    a->channels.assign(1, std::vector<float>(4000));
    for (uint64_t i = 0; i < 4000; ++i) {
      a->channels[0][i] = static_cast<float>(i) / 4000.0f;
    }
    a->frames = 4000;
    a->sampleRate = 48000.0;
    a->buildPlanes();
    kit->audio[0] = a;
    kit->state.slots[0].keyLow = 0;
    kit->state.slots[0].keyHigh = 127;
    kit->state.slots[0].rootKey = 60;
    kit->keymap.rebuild(kit->state);

    daw::SamplerRuntime rt;
    rt.configure(4, 48000.0);
    rt.setSnapshot(kit);
    Out o(1000);
    SamplerEvent ev[] = {on(0, 72, 1)};  // one octave above root
    rt.render(o.planes, 2, 1000, ev, 1);
    checkNear(o.l[500], a->channels[0][1000], 5e-3f,
              "an octave up reads at exactly twice the rate");
  }
  {
    // A FIXED-PITCH slot ignores the key entirely: pitchTrackMilli = 0 is how a drum stays a drum
    // across the keyboard.
    auto kit = makeKit(1, false, 4000);
    kit->state.slots[0].keyLow = 0;
    kit->state.slots[0].keyHigh = 127;
    kit->state.slots[0].rootKey = 60;
    kit->state.slots[0].pitchTrackMilli = 0;
    kit->keymap.rebuild(kit->state);
    auto a = std::make_shared<daw::SamplerSourceAudio>();
    a->channels.assign(1, std::vector<float>(4000));
    for (uint64_t i = 0; i < 4000; ++i) {
      a->channels[0][i] = static_cast<float>(i) / 4000.0f;
    }
    a->frames = 4000;
    a->sampleRate = 48000.0;
    a->buildPlanes();
    kit->audio[0] = a;
    daw::SamplerRuntime rt;
    rt.configure(4, 48000.0);
    rt.setSnapshot(kit);
    Out o(1000);
    SamplerEvent ev[] = {on(0, 84, 1)};  // two octaves up, and it must not matter
    rt.render(o.planes, 2, 1000, ev, 1);
    checkNear(o.l[500], a->channels[0][500], 5e-3f,
              "pitchTrackMilli=0 plays at unity whatever key is struck");
  }

  // ---- VELOCITY SCALES THE VOICE. Without it every hit is the same, which is the one thing a
  // tracker's velocity column is for.
  {
    auto kit = makeKit(1);
    daw::SamplerRuntime rt;
    rt.configure(4, 48000.0);
    rt.setSnapshot(kit);
    Out o(256);
    SamplerEvent ev[] = {on(0, 36, 1, 0, 0, /*vel=*/64)};
    rt.render(o.planes, 2, 256, ev, 1);
    checkNear(o.l[128], 64.0f / 127.0f, 1e-2f, "velocity scales the voice's gain");
  }

  // ---- NO SNAPSHOT AT ALL is inert. Reachable between device creation and the first publish.
  {
    daw::SamplerRuntime rt;
    rt.configure(8, 48000.0);
    Out o(256);
    SamplerEvent ev[] = {on(0, 36, 1)};
    rt.render(o.planes, 2, 256, ev, 1);  // must not crash
    check(rt.activeVoices() == 0, "a runtime with no snapshot renders nothing");
  }
  {
    // ...and a block with NO events still renders its sounding voices. A segmenting loop that
    // exits early when the event list is empty would drop every note's tail.
    auto kit = makeKit(2);
    daw::SamplerRuntime rt;
    rt.configure(8, 48000.0);
    rt.setSnapshot(kit);
    Out o1(256), o2(256);
    SamplerEvent ev[] = {on(0, 36, 1)};
    rt.render(o1.planes, 2, 256, ev, 1);
    rt.render(o2.planes, 2, 256, nullptr, 0);
    checkNear(o2.l[128], 1.0f, 1e-3f,
              "a block with NO events still renders the voices already sounding");
  }

  if (g_fail == 0) {
    std::printf("sampler_engine_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
