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
      CHECK(e.payload.note.reserved2 == 7);
  std::printf(fails ? "placement_adds_tests: %d failure(s)\n" : "placement_adds_tests: all passed\n", fails);
  return fails ? 1 : 0;
}
