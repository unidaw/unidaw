// THE 20-ROW ROUTING MATRIX, ITERATED — not restated.
//
// AE-P1.2 G2-B item 18, T-ROUTING-MATRIX:
//
//   "The implementation iterates the exact 5x4 routing_matrix: all 20 lane/kind rows produce the
//    declared validity, effect, and id result; each complementary rule is exercised one-sided,
//    exact-duplicate, source-cardinality, input-Track conflict, input-External conflict, Master
//    conflict, fan-in, and preFader canonicalization, with no implicit default case."
//
// ITERATES is the operative word, and it is why this file contains no table. The 20 rows live in
// apps/routing_matrix_generated.h, emitted from the frozen packet by the step-map checker and
// byte-compared against it whenever the packet pins resolve (the checker fails loudly when they
// do not). A fixture that spelled them out in C++ would be a second
// copy of the contract, and the two would agree right up until somebody edited one.
//
// The three columns are all asserted, because the record says the rows "fix validity, endpoint
// meaning, and id constraints" — a check that only tested `valid` would pass an implementation
// that accepted any ids at all.

#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "apps/routing_graph.h"
#include "apps/routing_matrix_generated.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("routing_graph_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

daw::TrackRouting emptyRouting() {
  daw::TrackRouting routing;
  // The product default routes audio to master. A matrix row is about ONE lane, so every other
  // lane starts at None — otherwise the default audio_out/Master would be a second declaration in
  // every row's fixture and half the conflict rules would fire before the row was tested.
  routing.audioOut = daw::TrackRoute{daw::TrackRouteKind::None, 0, 0};
  return routing;
}

daw::TrackRoute& laneOf(daw::TrackRouting& routing, daw::RoutingLane lane) {
  switch (lane) {
    case daw::RoutingLane::MidiIn: return routing.midiIn;
    case daw::RoutingLane::MidiOut: return routing.midiOut;
    case daw::RoutingLane::AudioIn: return routing.audioIn;
    case daw::RoutingLane::AudioOut: return routing.audioOut;
    case daw::RoutingLane::Sidechain: return routing.sidechain;
  }
  return routing.midiIn;
}

bool kindFromString(const std::string& text, daw::TrackRouteKind& out) {
  if (text == "None") { out = daw::TrackRouteKind::None; return true; }
  if (text == "Master") { out = daw::TrackRouteKind::Master; return true; }
  if (text == "Track") { out = daw::TrackRouteKind::Track; return true; }
  if (text == "ExternalInput") { out = daw::TrackRouteKind::ExternalInput; return true; }
  return false;
}

// Two tracks (0 and 1) and one registered external input, which is the smallest session in which
// every row is expressible: a Track row needs a non-self target that exists, an ExternalInput row
// needs a registered id.
struct Session {
  std::vector<daw::RoutingTrackInput> tracks;
  std::vector<daw::RoutingAuxChild> auxChildren;
  std::vector<uint32_t> inputs{7};
};

Session twoTracks() {
  Session session;
  session.tracks.push_back({0, emptyRouting()});
  session.tracks.push_back({1, emptyRouting()});
  return session;
}

bool compile(Session& session, daw::RoutingGraph& graph, daw::RoutingError& error) {
  error = daw::RoutingError{};
  return daw::compileRoutingGraph(session.tracks, session.auxChildren, session.inputs, graph,
                                  &error);
}

// The ids the row's `id_rule` column says are the legal ones.
daw::TrackRoute legalIdsFor(const std::string& idRule, daw::TrackRouteKind kind) {
  daw::TrackRoute route;
  route.kind = kind;
  if (idRule == "trackId=0,inputId=0") {
    route.trackId = 0;
    route.inputId = 0;
  } else if (idRule == "existing non-self trackId,inputId=0") {
    route.trackId = 1;  // track 0 declares; track 1 exists and is not self
    route.inputId = 0;
  } else if (idRule == "trackId=0,registered nonzero inputId") {
    route.trackId = 0;
    route.inputId = 7;
  }
  return route;
}

size_t edgesInto(const daw::RoutingGraph& graph, uint32_t destTrackId, daw::RoutingMedia media) {
  size_t n = 0;
  for (const auto& edge : graph.edges) {
    if (!edge.destIsMaster && edge.destTrackId == destTrackId && edge.media == media) {
      ++n;
    }
  }
  return n;
}

// Every edge whose source is `sourceTrackId`, in the given medium.
size_t edgesFrom(const daw::RoutingGraph& graph, uint32_t sourceTrackId, daw::RoutingMedia media) {
  size_t n = 0;
  for (const auto& edge : graph.edges) {
    if (edge.sourceTrackId == sourceTrackId && edge.media == media) {
      ++n;
    }
  }
  return n;
}

size_t edgesIntoMaster(const daw::RoutingGraph& graph, daw::RoutingMedia media) {
  size_t n = 0;
  for (const auto& edge : graph.edges) {
    if (edge.destIsMaster && edge.media == media) {
      ++n;
    }
  }
  return n;
}

size_t externalsFor(const daw::RoutingGraph& graph, uint32_t trackId, daw::RoutingMedia media,
                    uint32_t inputId) {
  size_t n = 0;
  for (const auto& source : graph.externals) {
    if (source.trackId == trackId && source.media == media && source.inputId == inputId) {
      ++n;
    }
  }
  return n;
}

// WHAT THE ROW'S `effect` SAYS THE GRAPH NOW CONTAINS. Track 0 is always the declaring track and
// track 1 the target, so each effect is a statement with a definite answer.
//
// Returns false for an effect string this fixture does not recognise, which the caller reports as
// a failure. The alternative — ignoring unknown effects — is how a table that gained a row would
// be reported as fully covered.
bool assertValidEffect(const std::string& effect, const daw::RoutingGraph& graph,
                       daw::RoutingLane lane, const std::string& where) {
  const daw::RoutingMedia media = daw::routingLaneMedia(lane);
  if (effect == "no_input_side_declaration" || effect == "no_output_side_declaration" ||
      effect == "no_key_source") {
    // THE ROW IS NOT ISOLATED FROM ITS SETUP HERE, and saying so is better than implying otherwise:
    // emptyRouting() already leaves every lane None{0,0}, so a None row's session is byte-identical
    // to the base session. The assertion is not vacuous — making a None audio_out imply Master
    // fires it — but it is the weakest branch in this function, and it does not distinguish the
    // three effect names from one another.
    //
    // What makes it worth keeping: it is the only branch that asserts the ABSENCE of an external
    // source, and `input_none` is the rule that "contributes no input-side constraint".
    expect(graph.edges.empty() && graph.externals.empty(),
           where + ": a None row declares NOTHING — no edge and no external source");
    // A SECOND ASSERTION WAS ADDED HERE AND REMOVED AGAIN. It checked that track 1 had no edges,
    // which `graph.edges.empty()` above already implies — edgesInto/edgesFrom iterate that same
    // vector — so its detection set was a strict subset and deleting it changed nothing anywhere
    // in the fixture. The comment beside it claimed it made the check "a statement about the row
    // rather than about the empty session"; it made no additional statement at all. An assertion
    // implied by its neighbour is worse than no assertion: it reads as strengthening.
    return true;
  }
  if (effect == "declare_one_track_source") {
    expect(edgesInto(graph, 0, media) == 1,
           where + ": the declaring track gains exactly one incoming edge");
    expect(edgesFrom(graph, 1, media) == 1, where + ": sourced from the track it named");
    return true;
  }
  if (effect == "declare_one_track_destination" ||
      effect == "declare_one_track_destination_without_direct_master") {
    expect(edgesFrom(graph, 0, media) == 1,
           where + ": the declaring track gains exactly one outgoing edge");
    expect(edgesInto(graph, 1, media) == 1, where + ": into the track it named");
    // "declare_one_track_destination_WITHOUT_DIRECT_MASTER" — the second half is the whole point:
    // routesToMaster conflated these, so a track feeding another track was ALSO summed into the
    // master mix.
    expect(edgesIntoMaster(graph, media) == 0,
           where + ": and does NOT also reach master");
    return true;
  }
  if (effect == "declare_master_sink_only") {
    expect(edgesIntoMaster(graph, media) == 1, where + ": one edge into master");
    expect(daw::routingGraphReachesMaster(graph, 0), where + ": from the declaring track");
    expect(edgesInto(graph, 1, media) == 0, where + ": and into no track — master ONLY");
    return true;
  }
  if (effect == "declare_external_midi_source" || effect == "declare_external_audio_source" ||
      effect == "declare_external_audio_key_source") {
    // THE EXTERNALS OUTPUT WAS READ BY NO TEST AT ALL until this existed: deleting the push that
    // produces it left the whole fixture passing.
    expect(externalsFor(graph, 0, media, 7) == 1,
           where + ": the declaring track gains exactly one external source on this medium");
    expect(graph.externals.size() == 1, where + ": and only that one");
    expect(graph.edges.empty(), where + ": an external source is not an edge — it has no source "
                                        "track to be delayed by a block");
    const daw::RoutingMedia expected =
        effect == "declare_external_midi_source" ? daw::RoutingMedia::Midi
        : effect == "declare_external_audio_source" ? daw::RoutingMedia::Audio
                                                    : daw::RoutingMedia::Sidechain;
    expect(!graph.externals.empty() && graph.externals.front().media == expected,
           where + ": on the medium the effect names — a lane whose media was wrong would still "
                   "produce 'an external source', just not this one");
    return true;
  }
  if (effect == "declare_one_additive_track_key_source") {
    expect(edgesInto(graph, 0, daw::RoutingMedia::Sidechain) == 1,
           where + ": one key edge into the declaring track");
    expect(edgesFrom(graph, 1, daw::RoutingMedia::Audio) == 0,
           where + ": and the source's AUDIO output is unchanged — the key edge is additive");
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- the 20 rows, one at a time
void everyRowProducesItsDeclaredResult() {
  expect(daw::generated::kRoutingMatrixRows == 20,
         "the frozen matrix must still be 5 lanes x 4 kinds");

  size_t exercised = 0;
  for (size_t i = 0; i < daw::generated::kRoutingMatrixRows; ++i) {
    const auto& row = daw::generated::kRoutingMatrix[i];
    const std::string where =
        std::string("row ") + row.lane + "/" + row.kind + " (" + row.effect + ")";

    daw::RoutingLane lane{};
    expect(daw::routingLaneFromString(row.lane, lane), where + ": the lane name must parse");
    daw::TrackRouteKind kind{};
    expect(kindFromString(row.kind, kind), where + ": the kind name must parse");

    // ---- VALIDITY, and for an invalid row the EFFECT --------------------------------------
    Session session = twoTracks();
    laneOf(session.tracks[0].routing, lane) =
        row.valid ? legalIdsFor(row.idRule, kind) : daw::TrackRoute{kind, 0, 0};
    if (!row.valid && kind == daw::TrackRouteKind::ExternalInput) {
      // An output-side ExternalInput row is rejected for BEING ExternalInput on that lane, so give
      // it ids that would be legal on an input lane. Otherwise the row could be refused by the id
      // rule instead and the effect assertion below would pass for the wrong reason.
      laneOf(session.tracks[0].routing, lane) = daw::TrackRoute{kind, 0, 7};
    }

    daw::RoutingGraph graph;
    daw::RoutingError error;
    const bool ok = compile(session, graph, error);
    expect(ok == row.valid, where + ": validity must match the frozen table");
    if (!row.valid) {
      expect(std::string(daw::routingErrorCodeToString(error.code)) == row.effect,
             where + ": the refusal must be the effect the table names, got '" +
                 daw::routingErrorCodeToString(error.code) + "'");
      expect(error.laneApplies && error.lane == lane,
             where + ": the refusal must name the lane that caused it");
      expect(std::string(row.idRule) == "rejected",
             where + ": an invalid row's id_rule is 'rejected'");
      ++exercised;
      continue;
    }

    // ---- THE EFFECT OF A VALID ROW, which is a statement about the GRAPH ---------------------
    //
    // The effect column was previously read only inside the `!row.valid` branch, so 14 of the 20
    // rows had their middle column ignored while the file's header claimed all three were
    // asserted. Eight distinct valid effects went unasserted, and the whole MIDI half of the table
    // with them.
    //
    // Every effect the frozen table names for a valid row is answered here. An effect this
    // fixture does not know is a FAILURE, not a skip — a table that grew a shape would otherwise
    // pass by not being recognised.
    expect(assertValidEffect(row.effect, graph, lane, where), where + ": effect asserted");

    // ---- THE ID RESULT. Every id_rule is asserted by VIOLATING it ---------------------------
    //
    // Asserting only that the legal ids compile would pass an implementation that ignored the ids
    // entirely — the row's third column would be untested while looking covered.
    const std::string idRule = row.idRule;
    if (idRule == "trackId=0,inputId=0") {
      Session bad = twoTracks();
      laneOf(bad.tracks[0].routing, lane) = daw::TrackRoute{kind, 1, 0};
      daw::RoutingGraph g;
      daw::RoutingError e;
      expect(!compile(bad, g, e), where + ": a nonzero trackId must be refused");
      expect(e.code == daw::RoutingErrorCode::NonZeroIdOnEmptyRow,
             where + ": ...as an empty-row id violation");

      Session bad2 = twoTracks();
      laneOf(bad2.tracks[0].routing, lane) = daw::TrackRoute{kind, 0, 7};
      expect(!compile(bad2, g, e), where + ": a nonzero inputId must be refused");
    } else if (idRule == "existing non-self trackId,inputId=0") {
      Session self = twoTracks();
      laneOf(self.tracks[0].routing, lane) = daw::TrackRoute{kind, 0, 0};
      daw::RoutingGraph g;
      daw::RoutingError e;
      expect(!compile(self, g, e), where + ": naming ITSELF must be refused");
      expect(e.code == daw::RoutingErrorCode::SelfTrack, where + ": ...as a self reference");

      Session missing = twoTracks();
      laneOf(missing.tracks[0].routing, lane) = daw::TrackRoute{kind, 99, 0};
      expect(!compile(missing, g, e), where + ": naming a track that does not exist is refused");
      expect(e.code == daw::RoutingErrorCode::UnknownTrack, where + ": ...as an unknown track");

      Session withInput = twoTracks();
      laneOf(withInput.tracks[0].routing, lane) = daw::TrackRoute{kind, 1, 7};
      expect(!compile(withInput, g, e), where + ": a Track row carrying an inputId is refused");
      expect(e.code == daw::RoutingErrorCode::InputIdOnTrackRow, where + ": ...as an id violation");
    } else if (idRule == "trackId=0,registered nonzero inputId") {
      Session zero = twoTracks();
      laneOf(zero.tracks[0].routing, lane) = daw::TrackRoute{kind, 0, 0};
      daw::RoutingGraph g;
      daw::RoutingError e;
      expect(!compile(zero, g, e), where + ": inputId 0 must be refused");
      expect(e.code == daw::RoutingErrorCode::UnregisteredInputId,
             where + ": ...as an unregistered input");

      Session unregistered = twoTracks();
      laneOf(unregistered.tracks[0].routing, lane) = daw::TrackRoute{kind, 0, 8};
      expect(!compile(unregistered, g, e),
             where + ": an inputId the session does not have must be refused");

      Session withTrack = twoTracks();
      laneOf(withTrack.tracks[0].routing, lane) = daw::TrackRoute{kind, 1, 7};
      expect(!compile(withTrack, g, e), where + ": an ExternalInput row naming a track is refused");
      // BY NAME. Every other code the compiler can raise is asserted by name; this one was the
      // exception, so reverting it to `nonzero_id_on_empty_row` — the wrong rule for a row whose
      // id_rule is "trackId=0,registered nonzero inputId" — left the fixture green.
      expect(e.code == daw::RoutingErrorCode::TrackIdOnExternalRow,
             where + ": ...as a track id on an external row, not as an empty-row violation");
    } else {
      expect(false, where + ": unrecognised id_rule '" + idRule + "' — the table grew a shape this "
                            "fixture does not exercise, which is a gap and not a pass");
    }
    ++exercised;
  }

  // "NO ROW MAY BE SKIPPED" WAS A TAUTOLOGY. `++exercised` sits on every path through the loop
  // body, so `exercised == kRoutingMatrixRows` held for any implementation and could not fail. It
  // is kept as a cheap guard against a future `break`, but it is not the coverage claim — the one
  // below is.
  expect(exercised == daw::generated::kRoutingMatrixRows,
         "every row of the frozen matrix must be exercised");

  // THE TABLE IS THE FULL CROSS PRODUCT, in order. `kRoutingMatrixRows == 20` would equally pass a
  // table with one cell duplicated and another missing — and the emitted lane and kind arrays,
  // which nothing read, are exactly what makes this checkable. Iterating the table is only
  // iterating the CONTRACT if the table is the whole matrix.
  constexpr size_t laneCount = sizeof(daw::generated::kRoutingLanes) / sizeof(const char*);
  constexpr size_t kindCount = sizeof(daw::generated::kRoutingKinds) / sizeof(const char*);
  expect(laneCount * kindCount == daw::generated::kRoutingMatrixRows,
         "the row count must be lanes x kinds, not merely 20");

  // AND THE VOCABULARY ITSELF MUST BE DISTINCT, which the cross-product check alone cannot see:
  // both arrays and the rows come from one JSON object, so a vocabulary corrupted CONSISTENTLY
  // passes. A reviewer duplicated "midi_in" in the lane array and replaced rows 4-7 with a second
  // copy of the midi_in block — four cells duplicated, the entire midi_out lane missing — and this
  // loop said the table was the whole matrix.
  for (size_t a = 0; a < laneCount; ++a) {
    for (size_t b = a + 1; b < laneCount; ++b) {
      expect(std::string(daw::generated::kRoutingLanes[a]) != daw::generated::kRoutingLanes[b],
             std::string("lanes ") + std::to_string(a) + " and " + std::to_string(b) +
                 " are the same name — a repeated lane means another lane is absent");
    }
  }
  for (size_t a = 0; a < kindCount; ++a) {
    for (size_t b = a + 1; b < kindCount; ++b) {
      expect(std::string(daw::generated::kRoutingKinds[a]) != daw::generated::kRoutingKinds[b],
             std::string("kinds ") + std::to_string(a) + " and " + std::to_string(b) +
                 " are the same name — a repeated kind means another kind is absent");
    }
  }
  for (size_t l = 0; l < laneCount; ++l) {
    for (size_t k = 0; k < kindCount; ++k) {
      const auto& row = daw::generated::kRoutingMatrix[l * kindCount + k];
      expect(std::string(row.lane) == daw::generated::kRoutingLanes[l] &&
                 std::string(row.kind) == daw::generated::kRoutingKinds[k],
             std::string("row ") + std::to_string(l * kindCount + k) + " must be " +
                 daw::generated::kRoutingLanes[l] + "/" + daw::generated::kRoutingKinds[k]);
    }
  }
}

// ---------------------------------------------------------------- the session's own shape
void aSessionMustBeDistinctTracksAndDistinctProjections() {
  // No clause names this, because a contract about routing BETWEEN tracks presumes tracks are
  // distinguishable. The consequence of not checking was concrete: `trackById` returns the first
  // match, so one authored document compiled to two different graphs depending on vector order.
  Session duplicate;
  daw::TrackRouting first = emptyRouting();
  first.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  first.preFaderSend = false;
  daw::TrackRouting second = emptyRouting();
  second.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  duplicate.tracks.push_back({1, first});
  duplicate.tracks.push_back({1, second});
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(!compile(duplicate, graph, e), "two tracks sharing an id must be refused");
  expect(e.code == daw::RoutingErrorCode::DuplicateTrackId, "...as a duplicate id");
  expect(!e.laneApplies,
         "and NOT against a lane — the session's shape is wrong, not one declaration on it");
  expect(e.trackId == 1, "naming the id that repeated");

  // AUX CHILDREN, which had no validation at all.
  Session repeated = twoTracks();
  repeated.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  repeated.auxChildren.push_back({0, 1});
  repeated.auxChildren.push_back({0, 1});
  expect(!compile(repeated, graph, e),
         "the same {parent, bus} twice must be refused — it projected the parent's edges twice, "
         "summing the stem into its destination twice");
  expect(e.code == daw::RoutingErrorCode::DuplicateAuxChild, "...as a duplicate projection");
  expect(!e.laneApplies && e.trackId == 0 && e.otherBusIndex == 1,
         "naming the parent and the BUS — a bus index reported as otherTrackId is a number that "
         "reads as a track and is not one");

  Session orphan = twoTracks();
  orphan.auxChildren.push_back({99, 1});
  expect(!compile(orphan, graph, e),
         "a stem whose parent does not exist must be refused rather than silently producing "
         "nothing — the same resolves-to-nothing no-op the record removes for external inputs");
  expect(e.code == daw::RoutingErrorCode::AuxChildHasNoParent, "...as a missing parent");

  // AND TWO DIFFERENT BUSES OF ONE PARENT ARE FINE, or the three refusals above would also be
  // satisfied by rejecting every aux child.
  Session fine = twoTracks();
  fine.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  fine.auxChildren.push_back({0, 1});
  fine.auxChildren.push_back({0, 2});
  expect(compile(fine, graph, e), "two distinct buses of one parent are legal");
}

// TESTED WITHOUT THE compile() HELPER, deliberately.
//
// `compile()` does `error = daw::RoutingError{}` before every call, so the fixture was supplying
// the precondition the API did not: removing the clear-on-success from compileRoutingGraph changed
// nothing anywhere. A check whose setup establishes what it tests is the recurring shape here, and
// the only way to see past it is to stop doing the setup.
void aSuccessfulCompileClearsTheError() {
  Session s = twoTracks();
  s.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  daw::RoutingGraph graph;

  daw::RoutingError stale;
  stale.code = daw::RoutingErrorCode::DuplicateTrackId;
  stale.trackId = 42;
  stale.laneApplies = true;
  stale.lane = daw::RoutingLane::AudioOut;
  // NOT zeroed here — that is the point.
  expect(daw::compileRoutingGraph(s.tracks, s.auxChildren, s.inputs, graph, &stale),
         "the session compiles");
  expect(stale.code == daw::RoutingErrorCode::None && stale.trackId == 0 && !stale.laneApplies,
         "and a successful compile CLEARS the error struct — a caller reusing one across calls, "
         "a retry loop or an unconditional log, would otherwise report a refusal that did not "
         "happen");
}

// A TRACK ID AT THE TOP OF THE RANGE IS A TRACK ID.
//
// `kMaster` used to be 0xFFFFFFFF inside the same set as real destinations, so a track whose
// audio_out named track 0xFFFFFFFF compiled to a MASTER edge: the named destination got nothing,
// master was summed with audio it should never have had, and the edge's latency charge fell from
// one block to zero — for the row whose frozen effect is
// `declare_one_track_destination_WITHOUT_DIRECT_MASTER`. The document parser accepts the id, so
// nothing upstream prevents it.
void aTopOfRangeTrackIdIsNotMaster() {
  constexpr uint32_t kTop = 0xFFFFFFFFu;
  Session s;
  s.tracks.push_back({0, emptyRouting()});
  s.tracks.push_back({kTop, emptyRouting()});
  s.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, kTop, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(s, graph, e), "routing to the highest expressible track id compiles");
  expect(graph.edges.size() == 1, "as one edge");
  expect(!graph.edges.front().destIsMaster,
         "which is NOT a master edge — the destination is a track");
  expect(graph.edges.front().destTrackId == kTop, "and it is the track that was named");
  expect(!daw::routingGraphReachesMaster(graph, 0),
         "and the source does not reach master, which is what its row forbids");
  expect(daw::routingEdgeLatencyBlocks(graph.edges.front()) == 1,
         "and it is charged a block, as a Track edge must be");

  // AND THE CARDINALITY RULE STILL SEES IT. Master-plus-Track went undetected when master wore a
  // track id.
  Session conflict;
  conflict.tracks.push_back({0, emptyRouting()});
  conflict.tracks.push_back({kTop, emptyRouting()});
  conflict.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  conflict.tracks[1].routing.audioIn = {daw::TrackRouteKind::Track, 0, 0};
  expect(!compile(conflict, graph, e),
         "Master plus a Track pull is still a cardinality failure at the top of the id range");
  expect(e.otherIsMaster, "and master is reported AS master");
}

void aRefusedCompileLeavesTheOutputUntouched() {
  // routing_graph.h: "COMPILE, or name the first rule that refuses." Step 4 "atomically publishes
  // the compiled graph", so a graph that was refused must not be sitting in the out-param looking
  // like one that was not. Externals used to be pushed before the conflict rules ran.
  daw::RoutingGraph graph;
  daw::RoutingError e;
  Session good = twoTracks();
  good.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  expect(compile(good, graph, e), "a legal session compiles");
  const size_t edgesBefore = graph.edges.size();
  expect(edgesBefore == 1, "and leaves one edge behind");

  Session bad = twoTracks();
  bad.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};
  bad.tracks[1].routing.audioIn = {daw::TrackRouteKind::ExternalInput, 0, 7};
  expect(!compile(bad, graph, e), "the conflicting session is refused");
  expect(graph.edges.size() == edgesBefore && graph.externals.empty(),
         "and the previous graph is UNTOUCHED — a failed compile must not half-fill its output");
}

// ---------------------------------------------------------------- the latency plan
void theLatencyPlanChargesOneBlockPerTrackEdgeAndNothingForMaster() {
  // `track_edge_latency_blocks: 1`; R-ROUTING-AUTHORITY: "The latency plan charges one block per
  // TRACK edge." A master edge is kind Master, not Track, and R-MASTER-CORRELATION says why that
  // reading is right: the master's ProcessBlock is sent "after matching track outputs produce
  // its input mix" — the same block.
  Session s = twoTracks();
  s.tracks.push_back({2, emptyRouting()});
  s.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};   // 0 -> 1, a Track edge
  s.tracks[1].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};  // 1 -> master
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(s, graph, e), "a track feeding a track feeding master compiles");

  expect(daw::routingInboundLatencyBlocks(graph, 1, daw::RoutingMedia::Audio) == 1,
         "the Track edge into track 1 charges exactly one block");
  // THE MASTER EDGE CHARGES NOTHING, asserted on the EDGE rather than through an aggregate. There
  // was a routingMasterInboundLatencyBlocks(); it could only ever return 0 — audio_out/Master is
  // the sole valid Master row and every master edge charges zero — so replacing its whole body
  // with `return 0;` passed the fixture. It is gone; this asserts the fact it was standing in for.
  for (const auto& edge : graph.edges) {
    if (edge.destIsMaster) {
      expect(daw::routingEdgeLatencyBlocks(edge) == 0,
             "a master edge charges NOTHING — the master consumes the block its sources just "
             "produced, so charging it would delay every track-to-master path for no reason");
    }
  }
  expect(daw::routingInboundLatencyBlocks(graph, 0, daw::RoutingMedia::Audio) == 0,
         "a track with no incoming edge is charged nothing");
  expect(daw::routingInboundLatencyBlocks(graph, 2, daw::RoutingMedia::Audio) == 0,
         "and neither is an unconnected one");

  // PER MEDIUM. Track 1's audio comes from outside — an external source is not an edge and is not
  // delayed — while its key comes from a track. A media-blind aggregate reported 1 for its AUDIO
  // input on the strength of the sidechain edge, which would have had step 4 delay the audio by a
  // block that does not exist.
  Session mixed = twoTracks();
  mixed.tracks[1].routing.audioIn = {daw::TrackRouteKind::ExternalInput, 0, 7};
  mixed.tracks[1].routing.sidechain = {daw::TrackRouteKind::Track, 0, 0};
  expect(compile(mixed, graph, e), "an external audio input with a track key compiles");
  expect(daw::routingInboundLatencyBlocks(graph, 1, daw::RoutingMedia::Audio) == 0,
         "its AUDIO input is not delayed — it has no incoming audio edge at all");
  expect(daw::routingInboundLatencyBlocks(graph, 1, daw::RoutingMedia::Sidechain) == 1,
         "while its KEY input is delayed a block, as a sidechain Track edge must be");

  for (const auto& edge : graph.edges) {
    expect(daw::routingEdgeLatencyBlocks(edge) == (edge.destIsMaster ? 0u : 1u),
           "every edge's charge is decided by whether it is a Track edge, and by nothing else");
  }

  // A CYCLE HAS A LATENCY, which is the point of defining the plan on edges: a longest path does
  // not exist here, and one hop still costs exactly one block.
  Session cycle = twoTracks();
  cycle.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};
  cycle.tracks[1].routing.audioOut = {daw::TrackRouteKind::Track, 0, 0};
  expect(compile(cycle, graph, e), "a cycle compiles");
  expect(daw::routingInboundLatencyBlocks(graph, 0, daw::RoutingMedia::Audio) == 1 &&
             daw::routingInboundLatencyBlocks(graph, 1, daw::RoutingMedia::Audio) == 1,
         "and both of its tracks are charged one block, with no path to be longest");
}

// ---------------------------------------------------------------- the lane vocabulary
void laneNamesAndMediaRoundTrip() {
  // These three are public API. Two of them had no caller at all, so `laneIsInputSide`'s claim
  // that sidechain "is an input lane with no opposite" was untested — flipping midi_out to true
  // changed nothing observable.
  for (size_t i = 0; i < sizeof(daw::generated::kRoutingLanes) / sizeof(const char*); ++i) {
    daw::RoutingLane lane{};
    expect(daw::routingLaneFromString(daw::generated::kRoutingLanes[i], lane),
           std::string("the frozen lane name '") + daw::generated::kRoutingLanes[i] + "' parses");
    expect(std::string(daw::routingLaneToString(lane)) == daw::generated::kRoutingLanes[i],
           "and round-trips back to the same frozen spelling");
  }
  daw::RoutingLane unknown{};
  expect(!daw::routingLaneFromString("control_out", unknown), "an unknown lane name is refused");

  expect(daw::routingLaneMedia(daw::RoutingLane::MidiIn) == daw::RoutingMedia::Midi &&
             daw::routingLaneMedia(daw::RoutingLane::MidiOut) == daw::RoutingMedia::Midi,
         "both MIDI lanes carry the MIDI medium");
  expect(daw::routingLaneMedia(daw::RoutingLane::AudioIn) == daw::RoutingMedia::Audio &&
             daw::routingLaneMedia(daw::RoutingLane::AudioOut) == daw::RoutingMedia::Audio,
         "both audio lanes carry the audio medium");
  expect(daw::routingLaneMedia(daw::RoutingLane::Sidechain) == daw::RoutingMedia::Sidechain,
         "and sidechain is its own medium, so a key edge is a separate contribution rather than "
         "an audio one. (An earlier version of this message claimed sharing audio's medium would "
         "let a key edge consume the source's one audio destination. It would not: destinationsOf "
         "is rebuilt per lane pair and the sidechain pair is non-complementary, so the cardinality "
         "loop never runs on it. The mutation IS caught, by nine assertions about media, latency "
         "and additivity — but not for the reason that was given.)");

  expect(daw::routingLaneIsInputSide(daw::RoutingLane::MidiIn) &&
             daw::routingLaneIsInputSide(daw::RoutingLane::AudioIn) &&
             daw::routingLaneIsInputSide(daw::RoutingLane::Sidechain),
         "the three input lanes are input lanes — sidechain among them, with no opposite side");
  expect(!daw::routingLaneIsInputSide(daw::RoutingLane::MidiOut) &&
             !daw::routingLaneIsInputSide(daw::RoutingLane::AudioOut),
         "and the two output lanes are not");
}

// ---------------------------------------------------------------- the normalization rules
// EVERY COMPLEMENTARY RULE, RUN ON BOTH COMPLEMENTARY MEDIA.
//
// These were written against audioIn/audioOut only, and a reviewer showed what that cost: NINE
// distinct sabotages of the MIDI half left the fixture passing — including turning off the
// midi_out/midi_in pair entirely, so a project where A.midiOut = Track(B) compiled to a graph with
// zero MIDI edges and B received no notes. The one MIDI test asserted nothing but "it compiled".
//
// `complementary_pairs: ["midi_out->midi_in", "audio_out->audio_in"]` — the rules are stated over
// media lanes, so they are tested over media lanes. Each rule below takes the pair it runs on and
// every caller runs it twice, which is why the media is a parameter rather than a literal.
struct Lanes {
  daw::RoutingLane in;
  daw::RoutingLane out;
  daw::RoutingMedia media;
  const char* name;
};

constexpr Lanes kMidi{daw::RoutingLane::MidiIn, daw::RoutingLane::MidiOut, daw::RoutingMedia::Midi,
                      "midi"};
constexpr Lanes kAudio{daw::RoutingLane::AudioIn, daw::RoutingLane::AudioOut,
                       daw::RoutingMedia::Audio, "audio"};

void oneSidedDeclarationsCreateAnEdge(const Lanes& L) {
  const std::string what = std::string("[") + L.name + "] ";
  // "one-sided Track declarations create an edge"
  Session fromOutput = twoTracks();
  laneOf(fromOutput.tracks[0].routing, L.out) = {daw::TrackRouteKind::Track, 1, 0};
  daw::RoutingGraph a;
  daw::RoutingError e;
  expect(compile(fromOutput, a, e), what + "an output-only declaration compiles");
  expect(edgesInto(a, 1, L.media) == 1, what + "and creates exactly one edge on ITS medium");
  expect(a.edges.size() == 1, what + "and no edge on any other");

  Session fromInput = twoTracks();
  laneOf(fromInput.tracks[1].routing, L.in) = {daw::TrackRouteKind::Track, 0, 0};
  daw::RoutingGraph b;
  expect(compile(fromInput, b, e), what + "an input-only declaration compiles");
  expect(edgesInto(b, 1, L.media) == 1, what + "and creates the SAME single edge");
  expect(b.edges.size() == a.edges.size(),
         what + "the two one-sided forms produce the same EDGE SET. (Their pre-fader flags can "
                "differ, and do — see preFaderSendIsCanonicalized...; which side declared an edge "
                "is not a property of whether it exists.)");
}

void exactDuplicatesCoalesceOnce(const Lanes& L) {
  const std::string what = std::string("[") + L.name + "] ";
  // "exact input/output duplicates coalesce once"
  Session both = twoTracks();
  laneOf(both.tracks[0].routing, L.out) = {daw::TrackRouteKind::Track, 1, 0};
  laneOf(both.tracks[1].routing, L.in) = {daw::TrackRouteKind::Track, 0, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(both, graph, e), what + "declaring the same edge from both sides is legal");
  expect(edgesInto(graph, 1, L.media) == 1,
         what + "and yields ONE edge, not two — a doubled edge would sum the source into the "
                "destination twice");
  expect(graph.edges.size() == 1, what + "and exactly one edge in the whole graph");
}

void aSourceHasAtMostOneDestination(const Lanes& L) {
  const std::string what = std::string("[") + L.name + "] ";
  // "each source has at most one Track or Master sink" — and both shapes the rule names.
  Session twoInputs = twoTracks();
  twoInputs.tracks.push_back({2, emptyRouting()});
  laneOf(twoInputs.tracks[1].routing, L.in) = {daw::TrackRouteKind::Track, 0, 0};
  laneOf(twoInputs.tracks[2].routing, L.in) = {daw::TrackRouteKind::Track, 0, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(!compile(twoInputs, graph, e),
         what + "two destinations pulling one source must fail compilation");
  expect(e.code == daw::RoutingErrorCode::SourceHasTwoDestinations,
         what + "...as a cardinality failure");
  expect(!e.otherIsMaster, what + "between two TRACKS, neither of them master");
  // NAMED BY MEDIUM, NOT BY LANE. The source here declares NOTHING — both pullers named it from
  // their input side — so reporting its output lane accused a declaration that does not exist.
  expect(!e.laneApplies && e.mediaApplies && e.media == L.media,
         what + "and the refusal names the MEDIUM, because no single declaration caused it");
  expect(e.trackId == 0, what + "naming the source that was pulled twice");

  // MASTER PLUS TRACK IS AUDIO-ONLY, because midi_out/Master is not a legal row at all — the
  // frozen table rejects it as `reject_missing_master_midi_sink`. Testing it on MIDI would be
  // testing the row rule a second time under a normalization name.
  if (L.media == daw::RoutingMedia::Audio) {
    Session masterPlusTrack = twoTracks();
    laneOf(masterPlusTrack.tracks[0].routing, L.out) = {daw::TrackRouteKind::Master, 0, 0};
    laneOf(masterPlusTrack.tracks[1].routing, L.in) = {daw::TrackRouteKind::Track, 0, 0};
    expect(!compile(masterPlusTrack, graph, e), what + "Master plus Track must fail compilation");
    expect(e.code == daw::RoutingErrorCode::SourceHasTwoDestinations,
           what + "...as a cardinality failure");
    expect(e.otherIsMaster,
           what + "and the other party is MASTER, said as such — reporting it as track id 0 made "
                  "it indistinguishable from a conflict with track 0, which is a real track here");
  }
}

void inputTrackConstrainsTheSourceSetExactly(const Lanes& L) {
  const std::string what = std::string("[") + L.name + "] ";
  // "therefore A.audioOut=B with B.audioIn=C is rejected when A differs from C."
  Session s = twoTracks();
  s.tracks.push_back({2, emptyRouting()});
  laneOf(s.tracks[0].routing, L.out) = {daw::TrackRouteKind::Track, 1, 0};   // A -> B
  laneOf(s.tracks[1].routing, L.in) = {daw::TrackRouteKind::Track, 2, 0};    // B <- C
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(!compile(s, graph, e),
         what + "A.out=B with B.in=C must be rejected for A != C");
  expect(e.code == daw::RoutingErrorCode::InputConflictsWithSource, what + "...as an input conflict");

  // AND THE SAME SHAPE WITH A == C IS LEGAL. Without this the check above would also pass an
  // implementation that rejected every input-side declaration.
  Session agreeing = twoTracks();
  laneOf(agreeing.tracks[0].routing, L.out) = {daw::TrackRouteKind::Track, 1, 0};
  laneOf(agreeing.tracks[1].routing, L.in) = {daw::TrackRouteKind::Track, 0, 0};
  expect(compile(agreeing, graph, e), what + "A.out=B with B.in=A is the coalescing case");
}

void inputExternalConflictsWithEveryTrackSource(const Lanes& L) {
  const std::string what = std::string("[") + L.name + "] ";
  // "input ExternalInput conflicts with every Track source"
  Session s = twoTracks();
  laneOf(s.tracks[0].routing, L.out) = {daw::TrackRouteKind::Track, 1, 0};
  laneOf(s.tracks[1].routing, L.in) = {daw::TrackRouteKind::ExternalInput, 0, 7};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(!compile(s, graph, e),
         what + "an external input and a Track source on one lane must conflict");
  expect(e.code == daw::RoutingErrorCode::InputConflictsWithSource, what + "...as an input conflict");

  // AND THE EXTERNAL ALONE IS FINE, on the right medium. This is what makes the refusal above a
  // conflict rather than a blanket rejection — and it reads graph.externals, which no test did.
  Session alone = twoTracks();
  laneOf(alone.tracks[1].routing, L.in) = {daw::TrackRouteKind::ExternalInput, 0, 7};
  expect(compile(alone, graph, e), what + "an external input with no Track source compiles");
  expect(externalsFor(graph, 1, L.media, 7) == 1,
         what + "and lands on the medium of the lane that declared it");
}

void inputNonePermitsFanIn(const Lanes& L) {
  const std::string what = std::string("[") + L.name + "] ";
  // "input None permits output-declared fan-in", and the fan-in ORDER is the frozen triple.
  Session s = twoTracks();
  s.tracks.push_back({2, emptyRouting()});
  s.tracks.push_back({3, emptyRouting()});
  laneOf(s.tracks[0].routing, L.out) = {daw::TrackRouteKind::Track, 3, 0};
  laneOf(s.tracks[2].routing, L.out) = {daw::TrackRouteKind::Track, 3, 0};
  laneOf(s.tracks[1].routing, L.out) = {daw::TrackRouteKind::Track, 3, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(s, graph, e),
         what + "three sources may fan into one destination whose input is None");
  expect(edgesInto(graph, 3, L.media) == 3, what + "and all three edges exist");

  // ASCENDING {sourceTrackId, sourceBus, channel}, asserted on the FLATTENED sequence rather than
  // on the sort key — the record states an order over contributions, not over a data structure.
  //
  // FILTERED BY MEDIUM AS WELL AS DESTINATION. The first version filtered on destination alone,
  // which passed only because it used one medium: the sort key orders by media BEFORE source, so a
  // destination fed by both a MIDI and an audio source would have made this predicate fail on a
  // perfectly legal graph.
  uint64_t previous = 0;
  bool first = true;
  for (const auto& edge : graph.edges) {
    if (edge.destIsMaster || edge.destTrackId != 3 || edge.media != L.media) {
      continue;
    }
    const uint64_t key = (static_cast<uint64_t>(edge.sourceTrackId) << 32) | edge.sourceBus;
    expect(first || key > previous,
           what + "fan-in must reduce in ascending {sourceTrackId, sourceBus}");
    previous = key;
    first = false;
  }
  expect(!first, what + "and the loop must have seen edges — an empty scan asserts nothing");
}

// TWO MEDIA INTO ONE DESTINATION, which is the case the media-blind predicate above would have
// failed. It is legal and it must stay legal.
void twoMediaMayShareOneDestination() {
  Session s = twoTracks();
  s.tracks.push_back({2, emptyRouting()});
  s.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 2, 0};
  s.tracks[1].routing.midiOut = {daw::TrackRouteKind::Track, 2, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(s, graph, e), "one destination may take audio from one track and MIDI from another");
  expect(edgesInto(graph, 2, daw::RoutingMedia::Audio) == 1, "the audio edge exists");
  expect(edgesInto(graph, 2, daw::RoutingMedia::Midi) == 1, "and so does the MIDI edge");
  expect(graph.edges.size() == 2, "and they are two edges, not one");

  // THE WHOLE EDGE VECTOR IS STRICTLY ORDERED, on the documented key and including the medium.
  //
  // This case is chosen so that media order and source order DISAGREE: the audio edge comes from
  // track 0 and the MIDI edge from track 1, so sorting by medium puts MIDI first and sorting by
  // source puts audio first. Without that disagreement the assertion passes either way — which is
  // what happened when the fixture only counted edges: dropping `media` from the sort comparator
  // left every test passing, and two edges that tie under the remaining key have an order
  // std::sort does not define at all. A published graph whose edge order varies run to run is not
  // a graph that was published atomically.
  bool strictlyIncreasing = true;
  for (size_t i = 1; i < graph.edges.size(); ++i) {
    const auto& a = graph.edges[i - 1];
    const auto& b = graph.edges[i];
    const auto key = [](const daw::RoutingEdge& edge) {
      return std::make_tuple(edge.destIsMaster, edge.destTrackId,
                             static_cast<int>(edge.media), edge.sourceTrackId, edge.sourceBus);
    };
    if (!(key(a) < key(b))) {
      strictlyIncreasing = false;
    }
  }
  expect(strictlyIncreasing,
         "the edge vector is strictly increasing on {destIsMaster, destTrackId, media, "
         "sourceTrackId, sourceBus} — a tie under that key is an order nothing defines");
  // GUARDED. Every other front() in this file is; this one was not, and the finding-10 fix is what
  // made it reachable — a refused compile now leaves `graph` UNTOUCHED, so any mutation that makes
  // this session refuse leaves it default-constructed and empty. The result was a SEGV that
  // aborted main() and silently skipped the nine test functions after this one, which is how a
  // reviewer gets sent to the wrong layer.
  expect(!graph.edges.empty() && graph.edges.front().media == daw::RoutingMedia::Midi,
         "and the MIDI edge sorts first here even though its source id is higher, which is what "
         "makes this case able to tell the two orderings apart");
}

void masterIsADestinationAndTrackOutputDoesNotReachIt() {
  // "audio_out Master reaches master only"; "declare_one_track_destination_without_direct_master"
  Session toMaster = twoTracks();
  toMaster.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(toMaster, graph, e), "audio_out Master compiles");
  expect(daw::routingGraphReachesMaster(graph, 0), "and the track reaches master");

  Session toTrack = twoTracks();
  toTrack.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};
  expect(compile(toTrack, graph, e), "audio_out Track compiles");
  expect(!daw::routingGraphReachesMaster(graph, 0),
         "and the track does NOT also reach master — routesToMaster conflated these, so a track "
         "feeding another track was summed into the mix twice");
}

void preFaderSendIsCanonicalizedTrueOffTheAudioOutTrackRow() {
  // "preFaderSend selects the pre- or post-fader N-1 signal only for audio_out Track ... and is
  // canonicalized true otherwise."
  Session onTheRow = twoTracks();
  onTheRow.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};
  onTheRow.tracks[0].routing.preFaderSend = false;
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(onTheRow, graph, e), "an audio_out Track row compiles");
  expect(!graph.edges.empty() && !graph.edges.front().preFaderSend,
         "and the authored false is CARRIED on that row");

  Session offTheRow = twoTracks();
  offTheRow.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  offTheRow.tracks[0].routing.preFaderSend = false;
  expect(compile(offTheRow, graph, e), "an audio_out Master row compiles");
  expect(!graph.edges.empty() && graph.edges.front().preFaderSend,
         "and preFaderSend is canonicalized TRUE, because the flag has no meaning off that row");

  // THE INPUT-DECLARED EDGE. B.audioIn = Track(A) while A.audioOut is None is not an `audio_out
  // Track` row, so A's authored flag does not apply to it.
  Session declaredByInput = twoTracks();
  declaredByInput.tracks[0].routing.preFaderSend = false;
  declaredByInput.tracks[1].routing.audioIn = {daw::TrackRouteKind::Track, 0, 0};
  expect(compile(declaredByInput, graph, e), "an input-declared edge compiles");
  expect(!graph.edges.empty() && graph.edges.front().preFaderSend,
         "and is canonically true — the source made no output declaration for the flag to describe");
}

void sidechainIsAdditiveAndDoesNotChangeTheSourceOutput() {
  // "A sidechain Track edge is additive key input and does not change source audioOut."
  Session s = twoTracks();
  s.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  s.tracks[1].routing.sidechain = {daw::TrackRouteKind::Track, 0, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(s, graph, e),
         "a track may key another while still going to master — the sidechain edge does not "
         "consume the source's one audio destination");
  expect(daw::routingGraphReachesMaster(graph, 0), "the source still reaches master");
  expect(edgesInto(graph, 1, daw::RoutingMedia::Sidechain) == 1, "and the key edge exists");

  // ONE SOURCE MAY KEY SEVERAL DESTINATIONS. One kick keying three compressors is the point of the
  // lane, so source cardinality deliberately does not apply to it.
  Session many = twoTracks();
  many.tracks.push_back({2, emptyRouting()});
  many.tracks.push_back({3, emptyRouting()});
  many.tracks[1].routing.sidechain = {daw::TrackRouteKind::Track, 0, 0};
  many.tracks[2].routing.sidechain = {daw::TrackRouteKind::Track, 0, 0};
  many.tracks[3].routing.sidechain = {daw::TrackRouteKind::Track, 0, 0};
  expect(compile(many, graph, e), "one source may key three destinations");
}

void cyclesAreLegalDelayedFeedback() {
  // "Track cycles are valid delayed feedback with one block per edge." Every edge delivers block
  // N-1, so a loop is a delay line and not an unresolvable order. A compiler that rejected cycles
  // would be refusing a feature.
  Session two = twoTracks();
  two.tracks[0].routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};
  two.tracks[1].routing.audioOut = {daw::TrackRouteKind::Track, 0, 0};
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(two, graph, e), "a two-track audio cycle must COMPILE");
  expect(graph.edges.size() == 2, "and keep both edges");

  Session three = twoTracks();
  three.tracks.push_back({2, emptyRouting()});
  three.tracks[0].routing.midiOut = {daw::TrackRouteKind::Track, 1, 0};
  three.tracks[1].routing.midiOut = {daw::TrackRouteKind::Track, 2, 0};
  three.tracks[2].routing.midiOut = {daw::TrackRouteKind::Track, 0, 0};
  expect(compile(three, graph, e), "a three-track MIDI cycle must COMPILE");
}

void auxChildrenAreDerivedProjectionsOfTheirParent() {
  // "Aux children are derived parent-owned output-bus projections rather than a sixth authored
  // lane and inherit the parent's exact identity."
  Session s = twoTracks();
  s.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  s.auxChildren.push_back({0, 1});
  s.auxChildren.push_back({0, 2});
  daw::RoutingGraph graph;
  daw::RoutingError e;
  expect(compile(s, graph, e), "a parent with two stems compiles");

  size_t buses[3] = {0, 0, 0};
  for (const auto& edge : graph.edges) {
    if (edge.destIsMaster && edge.sourceTrackId == 0 && edge.sourceBus < 3) {
      buses[edge.sourceBus]++;
    }
  }
  expect(buses[0] == 1 && buses[1] == 1 && buses[2] == 1,
         "the main output and both stems each reach master once, under the PARENT's track id and "
         "their own bus — a stem is not a track that declares things");

  // AN OUTPUT BUS IS AUDIO, and this test was single-media until a reviewer found what that hid.
  //
  // The parent below sends MIDI to another track and is keyed off by it. Projecting those edges
  // per bus delivered every note three times and tripled the key signal — a real defect that the
  // audio-only version of this test could not see, in the one place where the MIDI half of the
  // graph was actually wrong.
  Session everyMedium = twoTracks();
  everyMedium.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  everyMedium.tracks[0].routing.midiOut = {daw::TrackRouteKind::Track, 1, 0};
  everyMedium.tracks[1].routing.sidechain = {daw::TrackRouteKind::Track, 0, 0};
  everyMedium.auxChildren.push_back({0, 1});
  everyMedium.auxChildren.push_back({0, 2});
  expect(compile(everyMedium, graph, e), "a parent with stems, a MIDI send and a key compiles");
  size_t byMedia[3] = {0, 0, 0};
  for (const auto& edge : graph.edges) {
    if (edge.sourceTrackId != 0) {
      continue;
    }
    byMedia[static_cast<size_t>(edge.media)]++;
  }
  expect(byMedia[static_cast<size_t>(daw::RoutingMedia::Audio)] == 3,
         "the parent's audio reaches master once per bus — main plus two stems");
  expect(byMedia[static_cast<size_t>(daw::RoutingMedia::Midi)] == 1,
         "its MIDI send is ONE edge, not one per bus — a stem is a slice of the parent's audio "
         "output, not another copy of everything it sends");
  expect(byMedia[static_cast<size_t>(daw::RoutingMedia::Sidechain)] == 1,
         "and it is one key source, not three");
  for (const auto& edge : graph.edges) {
    expect(edge.sourceBus == 0 || edge.media == daw::RoutingMedia::Audio,
           "no non-audio edge may carry a bus index at all");
  }

  // A stem on bus 0 is not a stem: bus 0 IS the parent's main output and already has its edge.
  Session busZero = twoTracks();
  busZero.tracks[0].routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  busZero.auxChildren.push_back({0, 0});
  expect(compile(busZero, graph, e), "a bus-0 child compiles");
  size_t toMaster = 0;
  for (const auto& edge : graph.edges) {
    if (edge.destIsMaster && edge.sourceTrackId == 0) {
      ++toMaster;
    }
  }
  expect(toMaster == 1, "and adds no second edge — otherwise the parent is summed twice");
}

}  // namespace

int main() {
  everyRowProducesItsDeclaredResult();
  // BOTH COMPLEMENTARY MEDIA, every rule. `complementary_pairs` names two, and a fixture that ran
  // one of them left nine distinct MIDI sabotages passing.
  for (const Lanes& lanes : {kMidi, kAudio}) {
    oneSidedDeclarationsCreateAnEdge(lanes);
    exactDuplicatesCoalesceOnce(lanes);
    aSourceHasAtMostOneDestination(lanes);
    inputTrackConstrainsTheSourceSetExactly(lanes);
    inputExternalConflictsWithEveryTrackSource(lanes);
    inputNonePermitsFanIn(lanes);
  }
  twoMediaMayShareOneDestination();
  aSessionMustBeDistinctTracksAndDistinctProjections();
  aRefusedCompileLeavesTheOutputUntouched();
  aSuccessfulCompileClearsTheError();
  aTopOfRangeTrackIdIsNotMaster();
  theLatencyPlanChargesOneBlockPerTrackEdgeAndNothingForMaster();
  laneNamesAndMediaRoundTrip();
  masterIsADestinationAndTrackOutputDoesNotReachIt();
  preFaderSendIsCanonicalizedTrueOffTheAudioOutTrackRow();
  sidechainIsAdditiveAndDoesNotChangeTheSourceOutput();
  cyclesAreLegalDelayedFeedback();
  auxChildrenAreDerivedProjectionsOfTheirParent();

  if (failures != 0) {
    std::printf("routing_graph_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("routing_graph_tests: PASS\n");
  return 0;
}
