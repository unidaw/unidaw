// The field visitor: does one declaration of a struct's fields actually drive a traversal, and
// does FieldKind separate the things a differ must treat differently?
//
// THIS TESTS BEHAVIOUR, NOT A COUNT. An assertion like "Device has 6 fields" would be a second
// hand-maintained list of exactly the kind this mechanism exists to abolish — it would go stale
// the day a field is added, and updating it is the same forgettable step as updating the
// serializer. So the assertions here are about what a CONSUMER of the walk would conclude:
//   - a differ asks for the Authored fields and must NOT be handed a derived scan index;
//   - a differ asks which field IDENTIFIES a device and must be handed device_id;
//   - nesting must recurse, or a differ silently ignores a whole sub-struct (which is precisely
//     how the aux-child subset survived in two places).

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "apps/document_visitor_fields.h"

namespace {

// Collects what a differ would see: the names it would compare, and the name it would key on.
struct CollectingVisitor {
  std::vector<std::string> compared;   // Authored — the fields a change is measured in
  std::vector<std::string> identity;   // Identity — which object this is
  std::vector<std::string> ignored;    // Derived + Session — must not produce a diff

  template <typename T>
  void field(const char* name, T& value, daw::FieldKind kind = daw::FieldKind::Authored) {
    (void)value;
    switch (kind) {
      case daw::FieldKind::Authored: compared.emplace_back(name); break;
      case daw::FieldKind::Identity: identity.emplace_back(name); break;
      case daw::FieldKind::Derived:
      case daw::FieldKind::Session:  ignored.emplace_back(name); break;
    }
  }

  // Nested structs recurse. Without this a whole sub-struct is silently skipped and every
  // consumer of the walk inherits the hole.
  void field(const char* name, daw::TrackRoute& value,
             daw::FieldKind kind = daw::FieldKind::Authored) {
    (void)kind;
    const std::string prefix = std::string(name) + ".";
    CollectingVisitor inner;
    daw::visitFields(value, inner);
    for (const auto& n : inner.compared) { compared.push_back(prefix + n); }
    for (const auto& n : inner.identity) { identity.push_back(prefix + n); }
    for (const auto& n : inner.ignored)  { ignored.push_back(prefix + n); }
  }
};

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
  for (const auto& s : haystack) {
    if (s == needle) { return true; }
  }
  return false;
}

int failures = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  // ---- a differ must not see a derived scan index as a change -------------------------------
  {
    daw::Device device;
    CollectingVisitor v;
    daw::visitFields(device, v);

    expect(!contains(v.compared, "host_slot_index"),
           "host_slot_index must NOT be among the fields a differ compares — it is a cache index "
           "into THIS machine's plugin scan, recomputed by resolveDeviceSlot on every load, so "
           "comparing it reports changes nobody made whenever the cache differs");
    expect(contains(v.ignored, "host_slot_index"),
           "host_slot_index must be explicitly classified, not merely absent — an absent field is "
           "indistinguishable from one somebody forgot to declare");
    expect(contains(v.identity, "device_id"),
           "device_id must be the Identity field, so a differ can say 'device 7 changed' rather "
           "than 'the third element differs' — position is not identity");
    expect(!contains(v.compared, "device_id"),
           "device_id must not also be compared as content, or renumbering would read as an edit");
    expect(contains(v.compared, "bypass") && contains(v.compared, "kind"),
           "ordinary authored fields must be compared");
  }

  // ---- nesting must recurse -----------------------------------------------------------------
  {
    daw::TrackRouting routing;
    CollectingVisitor v;
    daw::visitFields(routing, v);

    expect(contains(v.compared, "audio_out.kind"),
           "the walk must RECURSE into TrackRoute — a sub-struct visited as an opaque leaf is a "
           "sub-struct no consumer can see inside, which is how a five-field subset survived in "
           "two separate functions");
    expect(contains(v.compared, "sidechain.track_id"),
           "every nested route must be reached, not just the first");
    expect(contains(v.compared, "pre_fader_send"),
           "scalars alongside nested structs must still be visited");
  }

  // ---- a route's track_id names ANOTHER object, so it is content, not identity ---------------
  {
    daw::TrackRoute route;
    CollectingVisitor v;
    daw::visitFields(route, v);

    expect(contains(v.compared, "track_id"),
           "TrackRoute::track_id must be COMPARED: re-pointing a send at another track is a real "
           "change. Marking it Identity would make a re-pointed send look like a different route "
           "instead of a changed one");
    expect(v.identity.empty(),
           "TrackRoute has no identity of its own — it is a value, addressed by its position in "
           "TrackRouting");
  }


  // ---- the five fields two separate producers dropped ---------------------------------------
  //
  // NOT a completeness count — this names the SPECIFIC fields that were lost, twice, in opposite
  // directions. captureDocument wrote only name/mixer/placements/ownedClips/automationClips for an
  // aux-child track, so a stem's chain and quantize were dropped on SAVE. AuxChildOverlay carried
  // the same five back in, so undo could not RESTORE them. Both are fixed in the engine; this
  // asserts the visitor cannot reintroduce the shape when the serializer is rebuilt on it.
  //
  // A named regression guard goes stale in the safe direction: if one of these is renamed the test
  // fails loudly and someone reads this comment. A "ProjectTrack has 18 fields" assertion would go
  // stale in the DANGEROUS direction — it passes while a field silently stops being visited.
  {
    daw::ProjectTrack track;
    CollectingVisitor v;
    daw::visitFields(track, v);

    for (const char* dropped : {"chain", "quantize", "collapsed", "routing", "mod_links",
                                "lines_per_beat", "harmony_quantize", "sound_addressed_only",
                                "allow_note_overlap"}) {
      expect(contains(v.compared, dropped),
             "a field the aux-child subset dropped must be visited — the handlers accept edits to "
             "every one of these on a stem, so anything the walk cannot see is work the user can "
             "do and the engine will silently discard");
      if (!contains(v.compared, dropped)) { std::printf("         missing: %s\n", dropped); }
    }

    // A stem is addressed by (parent, bus), not by trackId — trackId depends on how many slot
    // tracks happen to exist, so keying a differ on it would reattach a stem's material to the
    // wrong lane, which engine_save_project.cpp:250 already warns about.
    expect(contains(v.identity, "parent_id") && contains(v.identity, "aux_bus_index"),
           "a stem's identity is (parent_id, aux_bus_index), not its derived trackId");
    expect(!contains(v.compared, "aux_bus_index"),
           "aux_bus_index addresses the lane; comparing it as content would make a stem moved to "
           "another bus look like an edited stem rather than a different one");
  }


  // ---- the whole document -------------------------------------------------------------------
  //
  // ProjectDocument IS what a version is: DocumentHistory holds these and undo restores one. So
  // anything the walk cannot reach from here is outside undo NO MATTER how many handlers can edit
  // it — which was the original defect exactly, TrackStoreState carrying three fields while 55 of
  // 70 mutating commands had nowhere to record.
  {
    daw::ProjectDocument doc;
    CollectingVisitor v;
    daw::visitFields(doc, v);

    expect(contains(v.compared, "tracks") && contains(v.compared, "clips"),
           "the document walk must reach tracks and clips — they are the heavy leaves stage 3 will "
           "share, and a version that cannot see them is not a version of the song");
    expect(contains(v.compared, "harmony_timeline") && contains(v.compared, "tempo_map") &&
               contains(v.compared, "markers") && contains(v.compared, "time_sig_map"),
           "the song-scoped collections must be visited: these are SongStoreState's contents, and "
           "only ONE command (InsertRemoveTime) ever pushed an undo entry for any of them");
    expect(contains(v.compared, "seed"),
           "the generation seed is authored — a project that reloads with a different seed renders "
           "different notes from the same generators");
    expect(v.ignored.empty(),
           "nothing at document level is Derived or Session today; if that changes, the field "
           "needs a comment here saying why undo must not restore it");
  }

  if (failures != 0) {
    std::printf("document_visitor_tests: FAIL (%d)\n", failures);
    return 1;
  }
  std::printf("document_visitor_tests: PASS — one field declaration drives the walk, and "
              "FieldKind separates what a differ compares from what it must ignore\n");
  return 0;
}
