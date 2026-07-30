// M3.24: adds are THIS APPEARANCE's content — emitted once, placement-relative.
#include "apps/placement_flatten.h"
#include <cstdio>
using namespace daw;
static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++fails; } } while(0)
static MusicalEvent note(uint64_t t, uint8_t p, uint32_t id) {
  MusicalEvent e; e.nanotickOffset=t; e.type=MusicalEventType::Note;
  e.payload.note.pitch=p; e.payload.note.velocity=100; e.payload.note.column=0;
  e.payload.note.durationNanoticks=1000; e.payload.note.noteId=id; return e;
}
int main() {
  constexpr uint64_t Q=960000, BAR=4*Q;
  // A 1-BAR hat clip, placed across 4 bars: the clip loops 4 times.
  ProjectClip hat; hat.id=1; hat.lengthNanoticks=BAR; hat.kind=ClipKind::Symbolic;
  hat.clip.addEvent(note(0, 42, 1));
  ProjectPlacement pl; pl.clipId=1; pl.id=7; pl.at=0; pl.lengthNanoticks=4*BAR;
  // THE ADD: one extra hit, in bar 3 of this appearance only.
  pl.adds.push_back(note(2*BAR + Q, 46, 900));
  auto out = flattenPlacements({pl}, {hat}, 16*BAR);
  int base=0, added=0;
  for (const auto& e : out) {
    if (e.type != MusicalEventType::Note) continue;
    if (e.payload.note.pitch==42) ++base;
    if (e.payload.note.pitch==46) { ++added; CHECK(e.nanotickOffset == 2*BAR + Q); }
  }
  // The CLIP loops: 4 hits. The ADD does not: exactly 1, at the tick it was written.
  CHECK(base == 4);
  CHECK(added == 1);
  // An add past the placement's end has no time to sound in.
  ProjectPlacement past = pl; past.adds.clear();
  past.adds.push_back(note(9*BAR, 46, 901));
  int outside=0;
  for (const auto& e : flattenPlacements({past}, {hat}, 16*BAR))
    if (e.type==MusicalEventType::Note && e.payload.note.pitch==46) ++outside;
  CHECK(outside == 0);
  // A mute still silences a base note (unchanged behaviour).
  ProjectPlacement muted = pl; muted.adds.clear(); muted.mutes.push_back(1);
  int left=0;
  for (const auto& e : flattenPlacements({muted}, {hat}, 16*BAR))
    if (e.type==MusicalEventType::Note) ++left;
  CHECK(left == 0);
  // Provenance: an add carries the placement id, like a base note.
  for (const auto& e : flattenPlacements({pl}, {hat}, 16*BAR))
    if (e.type==MusicalEventType::Note && e.payload.note.pitch==46)
      CHECK(e.payload.note.placementId == 7);
  // An add on an AUDIO clip's placement still sounds. The audio skip used to `continue` over the
  // whole placement, so a note typed into an audio region's cell was accepted, saved and badged
  // as a local edit and scheduled nowhere — the note is the user's data and it was silently gone.
  // The clip's own events stay out of the symbolic scheduler (the audio path plays the region),
  // which is what the second CHECK pins: fixing the add must not start scheduling audio clips.
  {
    ProjectClip region;
    region.id = 5;
    region.name = "loop";
    region.lengthNanoticks = 4 * BAR;
    region.kind = ClipKind::Audio;
    region.audio.sourcePath = "/nonexistent/loop.wav";
    // A symbolic event ON the audio clip, which must NOT be scheduled — an audio clip's payload
    // is its region, and anything symbolic sitting in it is not this scheduler's business.
    region.clip.addEvent(note(0, 60, 500));
    ProjectPlacement onAudio;
    onAudio.clipId = 5;
    onAudio.id = 11;
    onAudio.at = 2 * BAR;
    onAudio.lengthNanoticks = 4 * BAR;
    onAudio.adds.push_back(note(Q, 46, 902));
    int addsHeard = 0, clipEvents = 0;
    for (const auto& e : flattenPlacements({onAudio}, {region}, 16 * BAR)) {
      if (e.type != MusicalEventType::Note) continue;
      if (e.payload.note.pitch == 46) {
        ++addsHeard;
        CHECK(e.nanotickOffset == 2 * BAR + Q);
        CHECK(e.payload.note.placementId == 11);
      }
      if (e.payload.note.pitch == 60) ++clipEvents;
    }
    CHECK(addsHeard == 1);
    CHECK(clipEvents == 0);
  }

  std::printf(fails ? "placement_adds_tests: %d failure(s)\n" : "placement_adds_tests: all passed\n", fails);
  return fails ? 1 : 0;
}
