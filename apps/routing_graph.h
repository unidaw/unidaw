#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "apps/track_routing.h"

// THE ONE DIRECTED ROUTING GRAPH, compiled from authored TrackRoute lanes.
//
// AE-P1.2 G2-B item 18, R-ROUTING-AUTHORITY: "Complete MIDI input/output, audio input/output,
// sidechain, aux-child, and pre-fader routing is normalized into one directed graph in the session
// ExecutionSnapshot revision."
//
// This file is that normalizer, and nothing else. It holds no engine state, reads no runtime, and
// touches no buffer: it takes the authored lanes of every track and returns either a graph or the
// first reason the lanes cannot be one. Step 4 compiles the result INTO the snapshot revision;
// until then nothing consumes it, which is deliberate — a compiler that is also its own consumer
// cannot be tested against the frozen table without the engine running.
//
// WHY A GRAPH AND NOT FIVE FIELDS. The authored form is per-track and one-sided: A can say
// "my audio goes to B", or B can say "my audio comes from A", and those are the same edge. Five
// independent lanes cannot express that they agree, so today they are read independently at three
// different places and the answers can differ. The matrix below decides every combination once.
//
// WHAT MAKES A NEW LANE IMPOSSIBLE TO HALF-ADD, since this file used to claim more than it had.
//
// `-Werror=switch` on this target makes a new enumerator a compile error in every SWITCH, and
// there are only switches here — the kind dispatch in phase 2 was an if/else chain that fell
// through in silence until a reviewer pointed at it. But the flag cannot see a hand-written table,
// and phase 1 iterates one. A reviewer added a sixth lane, fixed all seven switch errors, and got
// a clean build in which the new lane was never visited: an illegal declaration on it compiled and
// produced nothing.
//
// So the two tables are static_asserted against the FROZEN MATRIX's lane count rather than against
// the enum. That is the right authority and not merely a convenient one: the packet decides how
// many lanes the contract has, and a lane the contract does not name is one this compiler should
// not be resolving. A first attempt asserted against a hand-written `kRoutingLaneCount = 5`, which
// is the same defect one level up — the count did not change when the enum did, so the assert
// could not fire.
//
// THE TABLE IS FROZEN AND MACHINE-READ. The 20 lane-by-kind rows are not restated here; they live
// in the packet manifest's `routing_matrix` and the test fixture ITERATES them (T-ROUTING-MATRIX:
// "The implementation iterates the exact 5x4 routing_matrix ... with no implicit default case").
// The switches below are exhaustive over both enums with no `default:` arm.

namespace daw {

// The five authored lanes, in the manifest's order.
enum class RoutingLane : uint8_t {
  MidiIn = 0,
  MidiOut = 1,
  AudioIn = 2,
  AudioOut = 3,
  Sidechain = 4,
};


const char* routingLaneToString(RoutingLane lane);
bool routingLaneFromString(const std::string& text, RoutingLane& out);

// WHY A COMPILATION FAILED. Every code names a matrix row effect or a normalization rule, because
// "invalid routing" tells a caller nothing it can act on and the matrix already named all 20
// outcomes.
enum class RoutingErrorCode : uint16_t {
  None = 0,
  // ROW REJECTIONS. One per invalid `effect` in the frozen table — NOT one per shape. The table
  // names three different reasons a Master appears where it cannot, and collapsing them into one
  // "master is invalid here" code would make the compiler unable to produce "the declared effect"
  // that T-ROUTING-MATRIX asserts.
  MasterAsInputSource,       // rows: midi_in/Master, audio_in/Master
  MissingMasterMidiSink,     // row:  midi_out/Master
  MasterAsKeySource,         // row:  sidechain/Master
  InputKindAsOutputSink,     // rows: midi_out/ExternalInput, audio_out/ExternalInput
  // ID RULES. The table's `id_rule` column, which the record calls part of what the rows fix.
  UnknownTrack,              // Track rows: "existing non-self trackId"
  SelfTrack,                 // Track rows: "existing non-self trackId"
  NonZeroIdOnEmptyRow,       // None/Master rows: "trackId=0,inputId=0"
  InputIdOnTrackRow,         // Track rows: "...,inputId=0"
  UnregisteredInputId,       // ExternalInput rows: "registered nonzero inputId"
  TrackIdOnExternalRow,      // ExternalInput rows: "trackId=0,..."
  // NORMALIZATION, not rows: these have no cell of their own because they are about two
  // declarations meeting.
  SourceHasTwoDestinations,  // normalization.source_output_cardinality
  InputConflictsWithSource,  // normalization.input_track / input_external
  // THE SESSION ITSELF, rather than any one declaration. A graph is only well defined over a set
  // of distinct tracks and a set of distinct {parent, bus} projections; these say the input was
  // not one.
  DuplicateTrackId,
  DuplicateAuxChild,
  AuxChildHasNoParent,
};

const char* routingErrorCodeToString(RoutingErrorCode code);

enum class RoutingMedia : uint8_t {
  Midi = 0,
  Audio = 1,
  Sidechain = 2,
};

struct RoutingError {
  RoutingErrorCode code = RoutingErrorCode::None;
  // WHICH LANE, when ONE LANE'S DECLARATION caused it — and false whenever no single declaration
  // did. Three kinds of refusal are not about a lane:
  //
  //   the session-level three (DuplicateTrackId, DuplicateAuxChild, AuxChildHasNoParent), which
  //     used to stamp AudioOut — reading as "your audio output declaration is wrong" when the
  //     session's shape was what was wrong;
  //   SourceHasTwoDestinations, which is a conflict BETWEEN declarations. It named the output lane
  //     of the source, and the source frequently declares nothing at all: two tracks each pulling
  //     from track 0 produced "track 0's audio_out conflicts", with track 0's audioOut set to None
  //     and one of the two actual offenders never mentioned. `media` carries what that refusal can
  //     honestly say.
  //
  // DEFAULTS TO FALSE, so a field left unset reads as "no lane" rather than as "midi_in". The
  // fail-safe direction for a field whose whole purpose is to say when a lane is meaningless.
  bool laneApplies = false;
  RoutingLane lane = RoutingLane::MidiIn;
  // Set when a refusal is about a MEDIUM rather than one lane of it.
  bool mediaApplies = false;
  RoutingMedia media = RoutingMedia::Audio;
  uint32_t trackId = 0;       // the track whose declaration failed
  // THE OTHER PARTY, and what it IS depends on the code — which is why it is a variant rather than
  // one field that means three things:
  //   Source/input conflicts   -> otherTrackId, the conflicting track
  //   a Master conflict        -> otherIsMaster, because master is not a track id (it used to be
  //                              reported as trackId 0, which is a legal track in this fixture)
  //   the aux-child refusals   -> otherBusIndex, which is not a track id at all
  uint32_t otherTrackId = 0;
  uint32_t otherBusIndex = 0;
  bool otherIsMaster = false;
};

// ONE EDGE, ONE BLOCK LATE — BUT NOT EVERY EDGE.
//
// "Every MIDI, audio, and sidechain TRACK edge delivers the source's fully rendered block N-1 to
// destination block N", and `track_edge_latency_blocks` is 1. An earlier version of this comment
// said the charge applied "for every edge" and used that to argue no latency needed representing
// at all. Both halves were wrong:
//
//   * A MASTER edge is not a Track edge — R-ROUTING-AUTHORITY scopes the charge itself: "The
//     latency plan charges one block per TRACK edge", and audio_out/Master is kind Master.
//     R-MASTER-CORRELATION says why that is right rather than merely literal: the master's
//     ProcessBlock is sent "after matching track outputs produce its input mix" — the SAME
//     block. (That sentence is R-MASTER-CORRELATION's; three files here attributed it to
//     R-TRANSACTIONAL-EVENT-BATCH, whose own wording is "after their matching output resolves the
//     immutable master-input recipe". The substance held under either; the citation did not.)
//     A step-4 consumer that charged one block per element of `edges` would insert a spurious
//     block of latency on every track-to-master path.
//   * "The plan charges it per edge rather than storing it per edge" answered why there should be
//     no per-edge FIELD. It did not answer where the plan is, and the step map lists "the latency
//     plan" among this step's deliverables.
//
// So the charge is a named function of an edge (below), and the plan is computed from it.
struct RoutingEdge {
  RoutingMedia media = RoutingMedia::Audio;
  uint32_t sourceTrackId = 0;
  uint32_t sourceBus = 0;   // 0 = the track's main output; nonzero = an aux child's bus
  uint32_t destTrackId = 0; // meaningless when destIsMaster
  bool destIsMaster = false;
  bool preFaderSend = true; // canonical true except on an audio_out Track row

  // COMPARABLE, because a consumer has to be able to ask "is this the graph I compiled?". The
  // snapshot validator recompiles the graph from the authored plans and compares — the same
  // discipline the launch carrier gets — and that needs equality on the edge, not a hand-written
  // field-by-field loop at the call site which would silently stop covering a field added here.
  friend bool operator==(const RoutingEdge&, const RoutingEdge&) = default;
  friend bool operator!=(const RoutingEdge& a, const RoutingEdge& b) { return !(a == b); }
};

// NO `channel` FIELD, and that is a decision worth stating.
//
// The frozen order is "ascending {sourceTrackId, sourceBus, channel}". One edge per channel would
// satisfy it literally and be wrong for MIDI, where an edge carries a stream rather than a channel
// count. Instead the edges of one {destination, media} are ordered by {sourceTrackId, sourceBus},
// and a consumer iterates channels ascending inside each edge. Flattened, that IS ascending
// {sourceTrackId, sourceBus, channel} — identical, not merely equivalent, because the outer key is
// constant while the inner one runs.
//
// WHAT IS AND IS NOT TESTED, since an earlier version of this note claimed more. The fixture
// asserts the flattened {sourceTrackId, sourceBus} sequence is ascending, and the whole edge
// vector is strictly ordered on {destIsMaster, destTrackId, media, sourceTrackId, sourceBus}. The
// CHANNEL dimension of `fan_in_order` is argued here and tested by nothing — there is no channel
// in RoutingEdge to test. It becomes assertable at step 4, where a consumer iterates channels
// inside an edge and T-ROUTING-BLOCK-DETERMINISM can see the order it produces.

// AN EXTERNAL SOURCE is not an edge: it has no source track, so it cannot participate in the
// {sourceTrackId, sourceBus, channel} reduce order and it is not delayed by a block.
struct RoutingExternalSource {
  uint32_t trackId = 0;
  RoutingMedia media = RoutingMedia::Audio;
  uint32_t inputId = 0;

  friend bool operator==(const RoutingExternalSource&, const RoutingExternalSource&) = default;
  friend bool operator!=(const RoutingExternalSource& a, const RoutingExternalSource& b) {
    return !(a == b);
  }
};

struct RoutingGraph {
  // Sorted so that fan-in into one {destination, media} reduces in ascending
  // {sourceTrackId, sourceBus, channel} — "Fan-in reduces in ascending {sourceTrackId,
  // sourceBus, channel} order."
  std::vector<RoutingEdge> edges;
  std::vector<RoutingExternalSource> externals;
};

// THE LATENCY PLAN. `track_edge_latency_blocks: 1`, charged per TRACK edge and only there.
//
// DEFINED ON EDGES, NOT PATHS, and that is forced rather than chosen: "Track cycles are valid
// delayed feedback with one block per edge", so a longest-path latency does not exist for a legal
// graph. What a consumer can ask is what one hop costs, and how much delay arrives at a
// destination from its immediate sources — both of which are well defined with cycles present.
uint32_t routingEdgeLatencyBlocks(const RoutingEdge& edge);

// The delay, in blocks, on this destination's incoming edges OF ONE MEDIUM — 0 when it has none.
//
// PER MEDIUM, and the parameter is not decoration. Without it this maxed over every inbound edge
// regardless of medium, so a track whose audio came from an ExternalInput (no delay at all — an
// external source is not an edge) and whose sidechain came from a track reported 1: a step-4
// consumer aligning that track's AUDIO input by this number would over-delay it by a block on the
// strength of a key edge. The lanes are independent; their latencies are too.
uint32_t routingInboundLatencyBlocks(const RoutingGraph& graph, uint32_t destTrackId,
                                     RoutingMedia media);

// THERE IS NO MASTER EQUIVALENT, and its absence is the answer rather than an omission.
//
// One existed and it was a function that could only return 0: `audio_out/Master` is the sole valid
// Master row in the frozen table, and every master edge charges nothing. Adding a `media` argument
// to it — which a previous fix did, applying a real repair to both aggregates when only one needed
// it — made it a parameter that could not change any result. A reviewer replaced its whole body
// with `return 0;` and the fixture did not notice.
//
// The master's inbound delay is a property of the MATRIX, not of any graph: it is zero, always,
// and a function inviting a caller to ask per-graph would suggest otherwise.

// MASTER IS A DESTINATION, NOT A SECOND LIST. A `masterInclusion` vector beside `edges` would be
// the same fact stored twice, and the two would eventually disagree — which is the failure the
// whole record is removing, since routesToMaster is exactly such a second copy today. A track
// whose audio_out names another Track has no master edge at all
// ("declare_one_track_destination_without_direct_master"), so this query returns false for it.
bool routingGraphReachesMaster(const RoutingGraph& graph, uint32_t trackId);

// One track's authored lanes, plus what the compiler needs to resolve them. Aux children are NOT
// passed here: they are "derived parent-owned output-bus projections rather than a sixth authored
// lane", so they are supplied separately and inherit their parent's identity.
struct RoutingTrackInput {
  uint32_t trackId = 0;
  TrackRouting routing{};
};

struct RoutingAuxChild {
  uint32_t parentTrackId = 0;
  uint32_t busIndex = 0;  // "sourceBus participates in deterministic fan-in ordering"
};

// COMPILE, or name the first rule that refuses. Returns false with `error` filled.
//
// PRECONDITIONS, stated here because a caller reading only this block used to learn none of them:
//   * `tracks` holds no two entries with the same trackId. Refused as DuplicateTrackId — without
//     the check one authored document compiled to two different graphs depending on vector order.
//   * every `auxChildren` entry names a parent that appears in `tracks` (AuxChildHasNoParent), and
//     no {parent, bus} appears twice (DuplicateAuxChild).
//
// ON FAILURE `out` IS NOT TOUCHED. Not emptied — untouched, so whatever the caller had before is
// still there. Step 4 "atomically publishes the compiled graph", and a graph that was refused must
// not be sitting in the out-param looking like one that was not.
//
// `registeredInputIds` is the set of external input ids the session actually has; an
// ExternalInput row naming an id outside it fails ("registered nonzero inputId"), because a
// declaration that resolves to nothing is exactly the silent no-op this record removes.
//
// NO CYCLE CHECK, deliberately. "Track cycles are valid delayed feedback with one block per edge"
// — every edge delivers block N-1, so a loop is a delay line rather than an unresolvable order.
// A compiler that rejected cycles would be refusing a feature.
bool compileRoutingGraph(const std::vector<RoutingTrackInput>& tracks,
                         const std::vector<RoutingAuxChild>& auxChildren,
                         const std::vector<uint32_t>& registeredInputIds,
                         RoutingGraph& out,
                         RoutingError* error);

// The lane's media, and the lane's opposite side, as free functions over exhaustive switches.
//
// NOT "the only place a lane's meaning is written down", which is what this said and is not true:
// routingLaneFromString is an if/else chain mapping names to lanes, and the fixture carries its
// own laneOf switch. What IS true, and is the reason for the shape: a lane's meaning is written
// only in places -Werror=switch can see — with routingLaneFromString the one exception, since a
// name-to-value mapping has no switch to be exhaustive over. Adding a sixth lane produces seven
// compile errors and that function is not among them.
RoutingMedia routingLaneMedia(RoutingLane lane);
bool routingLaneIsInputSide(RoutingLane lane);

}  // namespace daw
