#include "apps/routing_graph.h"

// FOR THE LANE COUNT ONLY. The frozen matrix decides how many lanes the contract has, and the
// two hand-written tables below are asserted against it — see the note in routing_graph.h.
#include "apps/routing_matrix_generated.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace daw {
namespace {

constexpr size_t kFrozenLaneCount =
    sizeof(generated::kRoutingLanes) / sizeof(generated::kRoutingLanes[0]);

// A LANE SET AS A BITMASK, so the two tables below can be checked for MEMBERSHIP rather than for
// size. `kAllFrozenLanes` is every lane the frozen matrix names; a table matching it holds each of
// them exactly once, because a duplicate contributes no new bit and therefore leaves one missing.
constexpr uint32_t laneBit(RoutingLane lane) {
  return 1u << static_cast<uint32_t>(lane);
}

constexpr uint32_t kAllFrozenLanes = (1u << kFrozenLaneCount) - 1u;

template <size_t N>
constexpr uint32_t laneSetOf(const RoutingLane (&lanes)[N]) {
  uint32_t mask = 0;
  for (size_t i = 0; i < N; ++i) {
    mask |= laneBit(lanes[i]);
  }
  return mask;
}

// ONE MEDIA LANE PAIR: an input side and its complementary output side, or a lane with no opposite
// (sidechain names itself on both, which is what `complementary == false` means).
//
// At namespace scope rather than inside compileRoutingGraph, because a local class cannot hold the
// template that computes its lane set — and the set is the point: a size check passed a kPairs
// whose sidechain entry had been replaced by a second MIDI pair.
struct Pair {
  RoutingLane input;
  RoutingLane output;
  bool complementary;
};

// The union of both sides of every pair. A non-complementary entry names one lane twice and
// contributes one bit — exactly right, since it covers one lane.
template <size_t N>
constexpr uint32_t pairSetOf(const Pair (&pairs)[N]) {
  uint32_t mask = 0;
  for (size_t i = 0; i < N; ++i) {
    mask |= laneBit(pairs[i].input) | laneBit(pairs[i].output);
  }
  return mask;
}

// A refusal caused by ONE LANE's declaration.
bool fail(RoutingError* error, RoutingErrorCode code, RoutingLane lane, uint32_t trackId,
          uint32_t otherTrackId = 0) {
  if (error != nullptr) {
    RoutingError out;
    out.code = code;
    out.laneApplies = true;
    out.lane = lane;
    out.trackId = trackId;
    out.otherTrackId = otherTrackId;
    *error = out;
  }
  return false;
}

// A refusal caused by a source having two destinations, one of which may be MASTER. Master is not
// a track id, and reporting it as 0 made a Master conflict indistinguishable from a conflict with
// track 0 — a legal track. It names the MEDIUM rather than a lane: see RoutingError.
bool failCardinality(RoutingError* error, RoutingMedia media, uint32_t sourceTrackId,
                     bool otherIsMaster, uint32_t otherTrackId) {
  if (error != nullptr) {
    RoutingError out;
    out.code = RoutingErrorCode::SourceHasTwoDestinations;
    // NO LANE. The source often declares nothing — two tracks each naming it from their input side
    // is the common shape — so naming its output lane accused a declaration that does not exist.
    out.laneApplies = false;
    out.mediaApplies = true;
    out.media = media;
    out.trackId = sourceTrackId;
    out.otherIsMaster = otherIsMaster;
    out.otherTrackId = otherIsMaster ? 0 : otherTrackId;
    *error = out;
  }
  return false;
}

// A refusal about the SESSION's shape rather than any lane. `laneApplies` is false: naming a lane
// here would read as "your audio output declaration is wrong", which is not what happened.
bool failSession(RoutingError* error, RoutingErrorCode code, uint32_t trackId,
                 uint32_t busIndex = 0) {
  if (error != nullptr) {
    RoutingError out;
    out.code = code;
    out.laneApplies = false;
    out.trackId = trackId;
    out.otherBusIndex = busIndex;
    *error = out;
  }
  return false;
}

// THE 20 ROWS, as one exhaustive switch per question. The table itself is frozen in the packet and
// iterated by the fixture; what lives here is the RULE, and the fixture proves the two agree row by
// row. No `default:` arm anywhere below — T-ROUTING-MATRIX says "with no implicit default case",
// and without one a new lane or kind is a compile error instead of a silent fallthrough.

RoutingMedia laneMedia(RoutingLane lane) {
  switch (lane) {
    case RoutingLane::MidiIn: return RoutingMedia::Midi;
    case RoutingLane::MidiOut: return RoutingMedia::Midi;
    case RoutingLane::AudioIn: return RoutingMedia::Audio;
    case RoutingLane::AudioOut: return RoutingMedia::Audio;
    case RoutingLane::Sidechain: return RoutingMedia::Sidechain;
  }
  return RoutingMedia::Audio;
}

bool laneIsInputSide(RoutingLane lane) {
  switch (lane) {
    case RoutingLane::MidiIn: return true;
    case RoutingLane::MidiOut: return false;
    case RoutingLane::AudioIn: return true;
    case RoutingLane::AudioOut: return false;
    // SIDECHAIN IS AN INPUT LANE WITH NO OPPOSITE. It names where the key signal comes FROM, and
    // there is no `sidechain_out` for a source to declare — which is why it takes no part in the
    // complementary-pair rules below and why `source_output_cardinality` does not apply to it.
    case RoutingLane::Sidechain: return true;
  }
  return true;
}

// Row validity, per kind, and the effect name the frozen table gives the rejection.
bool laneAcceptsMaster(RoutingLane lane, RoutingErrorCode& why) {
  switch (lane) {
    case RoutingLane::MidiIn:
      why = RoutingErrorCode::MasterAsInputSource;      // reject_master_as_input_source
      return false;
    case RoutingLane::MidiOut:
      why = RoutingErrorCode::MissingMasterMidiSink;    // reject_missing_master_midi_sink
      return false;
    case RoutingLane::AudioIn:
      why = RoutingErrorCode::MasterAsInputSource;      // reject_master_as_input_source
      return false;
    case RoutingLane::AudioOut:
      return true;                                      // declare_master_sink_only
    case RoutingLane::Sidechain:
      why = RoutingErrorCode::MasterAsKeySource;        // reject_master_as_key_source
      return false;
  }
  why = RoutingErrorCode::MasterAsInputSource;
  return false;
}

bool laneAcceptsExternalInput(RoutingLane lane) {
  switch (lane) {
    case RoutingLane::MidiIn: return true;      // declare_external_midi_source
    case RoutingLane::MidiOut: return false;    // reject_input_kind_as_output_sink
    case RoutingLane::AudioIn: return true;     // declare_external_audio_source
    case RoutingLane::AudioOut: return false;   // reject_input_kind_as_output_sink
    case RoutingLane::Sidechain: return true;   // declare_external_audio_key_source
  }
  return false;
}

const TrackRoute& laneOf(const TrackRouting& routing, RoutingLane lane) {
  switch (lane) {
    case RoutingLane::MidiIn: return routing.midiIn;
    case RoutingLane::MidiOut: return routing.midiOut;
    case RoutingLane::AudioIn: return routing.audioIn;
    case RoutingLane::AudioOut: return routing.audioOut;
    case RoutingLane::Sidechain: return routing.sidechain;
  }
  return routing.midiIn;
}

// ONE CELL OF THE MATRIX: is this (lane, kind) legal, and do its ids obey the row's id_rule?
//
// The id_rule column is not decoration — the record says the 20 rows "fix validity, endpoint
// meaning, and id constraints", and T-ROUTING-MATRIX asserts all three. A `None` row carrying a
// leftover trackId is a declaration that reads as nothing and compares as something.
bool validateCell(const RoutingTrackInput& track, RoutingLane lane,
                  const std::vector<RoutingTrackInput>& all,
                  const std::vector<uint32_t>& registeredInputIds, RoutingError* error) {
  const TrackRoute& route = laneOf(track.routing, lane);
  switch (route.kind) {
    case TrackRouteKind::None:
      // id_rule "trackId=0,inputId=0"
      if (route.trackId != 0 || route.inputId != 0) {
        return fail(error, RoutingErrorCode::NonZeroIdOnEmptyRow, lane, track.trackId);
      }
      return true;

    case TrackRouteKind::Master: {
      RoutingErrorCode why = RoutingErrorCode::MasterAsInputSource;
      if (!laneAcceptsMaster(lane, why)) {
        return fail(error, why, lane, track.trackId);
      }
      // id_rule "trackId=0,inputId=0" — master is one endpoint, so nothing identifies it.
      if (route.trackId != 0 || route.inputId != 0) {
        return fail(error, RoutingErrorCode::NonZeroIdOnEmptyRow, lane, track.trackId);
      }
      return true;
    }

    case TrackRouteKind::Track: {
      // id_rule "existing non-self trackId,inputId=0"
      if (route.trackId == track.trackId) {
        return fail(error, RoutingErrorCode::SelfTrack, lane, track.trackId, route.trackId);
      }
      const bool exists = std::any_of(all.begin(), all.end(),
                                      [&](const RoutingTrackInput& candidate) {
                                        return candidate.trackId == route.trackId;
                                      });
      if (!exists) {
        return fail(error, RoutingErrorCode::UnknownTrack, lane, track.trackId, route.trackId);
      }
      if (route.inputId != 0) {
        return fail(error, RoutingErrorCode::InputIdOnTrackRow, lane, track.trackId);
      }
      return true;
    }

    case TrackRouteKind::ExternalInput: {
      if (!laneAcceptsExternalInput(lane)) {
        return fail(error, RoutingErrorCode::InputKindAsOutputSink, lane, track.trackId);
      }
      // id_rule "trackId=0,registered nonzero inputId" — its OWN code, because this row is not an
      // empty one. Reporting `nonzero_id_on_empty_row` for a populated external declaration named
      // a rule the row does not have.
      if (route.trackId != 0) {
        return fail(error, RoutingErrorCode::TrackIdOnExternalRow, lane, track.trackId);
      }
      if (route.inputId == 0 ||
          std::find(registeredInputIds.begin(), registeredInputIds.end(), route.inputId) ==
              registeredInputIds.end()) {
        return fail(error, RoutingErrorCode::UnregisteredInputId, lane, track.trackId);
      }
      return true;
    }
  }
  return fail(error, RoutingErrorCode::UnknownTrack, lane, track.trackId);
}

}  // namespace

RoutingMedia routingLaneMedia(RoutingLane lane) { return laneMedia(lane); }
bool routingLaneIsInputSide(RoutingLane lane) { return laneIsInputSide(lane); }

const char* routingLaneToString(RoutingLane lane) {
  switch (lane) {
    case RoutingLane::MidiIn: return "midi_in";
    case RoutingLane::MidiOut: return "midi_out";
    case RoutingLane::AudioIn: return "audio_in";
    case RoutingLane::AudioOut: return "audio_out";
    case RoutingLane::Sidechain: return "sidechain";
  }
  return "midi_in";
}

bool routingLaneFromString(const std::string& text, RoutingLane& out) {
  if (text == "midi_in") { out = RoutingLane::MidiIn; return true; }
  if (text == "midi_out") { out = RoutingLane::MidiOut; return true; }
  if (text == "audio_in") { out = RoutingLane::AudioIn; return true; }
  if (text == "audio_out") { out = RoutingLane::AudioOut; return true; }
  if (text == "sidechain") { out = RoutingLane::Sidechain; return true; }
  return false;
}

const char* routingErrorCodeToString(RoutingErrorCode code) {
  switch (code) {
    case RoutingErrorCode::None: return "none";
    case RoutingErrorCode::MasterAsInputSource: return "reject_master_as_input_source";
    case RoutingErrorCode::MissingMasterMidiSink: return "reject_missing_master_midi_sink";
    case RoutingErrorCode::MasterAsKeySource: return "reject_master_as_key_source";
    case RoutingErrorCode::InputKindAsOutputSink: return "reject_input_kind_as_output_sink";
    case RoutingErrorCode::UnknownTrack: return "unknown_track";
    case RoutingErrorCode::SelfTrack: return "self_track";
    case RoutingErrorCode::NonZeroIdOnEmptyRow: return "nonzero_id_on_empty_row";
    case RoutingErrorCode::InputIdOnTrackRow: return "input_id_on_track_row";
    case RoutingErrorCode::UnregisteredInputId: return "unregistered_input_id";
    case RoutingErrorCode::TrackIdOnExternalRow: return "track_id_on_external_row";
    case RoutingErrorCode::DuplicateTrackId: return "duplicate_track_id";
    case RoutingErrorCode::DuplicateAuxChild: return "duplicate_aux_child";
    case RoutingErrorCode::AuxChildHasNoParent: return "aux_child_has_no_parent";
    case RoutingErrorCode::SourceHasTwoDestinations: return "source_has_two_destinations";
    case RoutingErrorCode::InputConflictsWithSource: return "input_conflicts_with_source";
  }
  return "none";
}

// THE LATENCY PLAN. `track_edge_latency_blocks: 1`, and only on a Track edge.
//
// A master edge costs NOTHING here. R-ROUTING-AUTHORITY scopes the charge — "The latency plan
// charges one block per TRACK edge" — and audio_out/Master is kind Master, not Track.
// R-MASTER-CORRELATION says why that is the right reading and not merely the literal one: the
// master's ProcessBlock is sent "after matching track outputs produce its input mix", so the
// master consumes the same block its sources just produced. Charging it would delay every
// track-to-master path for no reason, and would do it invisibly — which is why this is a function
// rather than a sentence a consumer has to remember.
//
// A SIDECHAIN edge DOES cost a block: the record names it among the delayed ones — "Every MIDI,
// audio, and sidechain Track edge delivers the source's fully rendered block N-1".
uint32_t routingEdgeLatencyBlocks(const RoutingEdge& edge) {
  return edge.destIsMaster ? 0u : 1u;
}

namespace {
uint32_t inboundLatency(const RoutingGraph& graph, uint32_t destTrackId, RoutingMedia media) {
  uint32_t blocks = 0;
  for (const auto& edge : graph.edges) {
    if (edge.destIsMaster || edge.media != media || edge.destTrackId != destTrackId) {
      continue;
    }
    // MAX, not sum: the edges are parallel contributions into one destination, not a chain. They
    // all carry the same charge today, so this is a max over equal values — written as a max
    // anyway, because "they are all 1" is a fact about the current table rather than about the
    // shape of the question.
    blocks = std::max(blocks, routingEdgeLatencyBlocks(edge));
  }
  return blocks;
}
}  // namespace

uint32_t routingInboundLatencyBlocks(const RoutingGraph& graph, uint32_t destTrackId,
                                     RoutingMedia media) {
  return inboundLatency(graph, destTrackId, media);
}

bool routingGraphReachesMaster(const RoutingGraph& graph, uint32_t trackId) {
  return std::any_of(graph.edges.begin(), graph.edges.end(), [&](const RoutingEdge& edge) {
    return edge.destIsMaster && edge.media == RoutingMedia::Audio &&
           edge.sourceTrackId == trackId && edge.sourceBus == 0;
  });
}

bool compileRoutingGraph(const std::vector<RoutingTrackInput>& tracks,
                         const std::vector<RoutingAuxChild>& auxChildren,
                         const std::vector<uint32_t>& registeredInputIds,
                         RoutingGraph& out,
                         RoutingError* error) {
  // WRITTEN INTO A LOCAL AND SWAPPED IN ON SUCCESS. `out` used to be cleared here and then filled
  // as the phases ran, so a compile that FAILED left a partially populated graph in it — externals
  // resolved, edges half-built. Step 4 "atomically publishes the compiled graph"; a caller that
  // published the out-param without checking the bool would publish one that was refused. A
  // failure now leaves `out` untouched, which is the only state a caller cannot misread.
  // AND THE ERROR IS CLEARED ON SUCCESS. It was left untouched, so a caller reusing one struct —
  // a retry loop, or an unconditional diagnostic log — read a stale refusal after a compile that
  // worked. The fixture could not see it because its own helper zeroed the struct first: the test
  // was supplying the precondition the API did not.
  if (error != nullptr) {
    *error = RoutingError{};
  }
  RoutingGraph built;
  const auto succeed = [&]() {
    out = std::move(built);
    return true;
  };

  // ---- PHASE 0: the session must be a set of distinct tracks and distinct projections ---------
  //
  // No clause names this, because a contract about routing between tracks presumes tracks are
  // distinguishable. The compiler validates self-reference, unknown targets, stray inputIds and
  // unregistered externals, and used to accept duplicates in SILENCE — with a real consequence:
  // `trackById` returns the first match, so two tracks sharing an id compiled to a graph that
  // depended on their order in the vector. Same authored document, two different graphs.
  {
    std::set<uint32_t> seenTracks;
    for (const auto& track : tracks) {
      if (!seenTracks.insert(track.trackId).second) {
        return failSession(error, RoutingErrorCode::DuplicateTrackId, track.trackId);
      }
    }
    // AND THE AUX CHILDREN, which had no validation at all. A repeated {parent, bus} projected the
    // parent's edges TWICE — summing the stem into its destination twice, which is exactly the
    // failure `auxChildrenAreDerivedProjectionsOfTheirParent` guards against for bus 0. A child
    // naming a parent that does not exist produced nothing at all, silently: the same
    // resolves-to-nothing no-op this record removes for external inputs.
    std::set<std::pair<uint32_t, uint32_t>> seenChildren;
    for (const auto& child : auxChildren) {
      if (!seenChildren.insert({child.parentTrackId, child.busIndex}).second) {
        return failSession(error, RoutingErrorCode::DuplicateAuxChild, child.parentTrackId,
                           child.busIndex);
      }
      if (seenTracks.count(child.parentTrackId) == 0) {
        return failSession(error, RoutingErrorCode::AuxChildHasNoParent, child.parentTrackId,
                           child.busIndex);
      }
    }
  }

  // ---- PHASE 1: every cell of the 5x4 matrix, before any two declarations are compared --------
  //
  // A conflict rule reasons about what two lanes MEAN, so it can only run once each lane is known
  // to mean something. Running them together would report "source has two destinations" for a pair
  // where one of the two is not a legal declaration at all.
  // THE LANES PHASE 1 ACTUALLY VISITS. A hand-written array is invisible to -Werror=switch, so a
  // sixth lane added to the enum — with all six switch errors dutifully fixed — left this at five
  // entries and phase 1 never visited the new lane at all: an illegal declaration on it compiled
  // clean and produced nothing.
  static constexpr RoutingLane kLanes[] = {RoutingLane::MidiIn, RoutingLane::MidiOut,
                                           RoutingLane::AudioIn, RoutingLane::AudioOut,
                                           RoutingLane::Sidechain};
  // MEMBERSHIP, NOT CARDINALITY — and getting here took three tries, each of which counted
  // something instead of naming it. First a hand-written `kRoutingLaneCount = 5`, which did not
  // change when the enum did. Then the frozen count, which cannot fire when a lane is DUPLICATED:
  // a reviewer replaced MidiOut with a second MidiIn, leaving five entries with midi_out never
  // visited, and it compiled clean. A count cannot see identity. This does.
  static_assert(laneSetOf(kLanes) == kAllFrozenLanes,
                "phase 1 must visit every lane the FROZEN matrix names, each exactly once");
  for (const auto& track : tracks) {
    for (RoutingLane lane : kLanes) {
      if (!validateCell(track, lane, tracks, registeredInputIds, error)) {
        return false;
      }
    }
  }

  // ---- PHASE 2: resolve each medium's edges ---------------------------------------------------
  //
  // "For complementary MIDI and audio lanes, one-sided Track declarations create an edge, exact
  // input/output duplicates coalesce once, each source has at most one Track or Master sink, input
  // None permits output-declared fan-in, input Track(source) constrains the final source set to
  // exactly that source, and input ExternalInput conflicts with every Track source."

  static constexpr Pair kPairs[] = {
      {RoutingLane::MidiIn, RoutingLane::MidiOut, true},
      {RoutingLane::AudioIn, RoutingLane::AudioOut, true},
      // "A sidechain Track edge is additive key input and does not change source audioOut" — so it
      // is resolved on its own, from the input side only, and does not join the audio cardinality.
      {RoutingLane::Sidechain, RoutingLane::Sidechain, false},
  };
  // EVERY LANE APPEARS IN EXACTLY ONE PAIR, checked by NAME. A lane missing from here is a lane
  // whose declarations validate in phase 1 and then resolve to no edge in phase 2 — accepted and
  // silently inert, which is the failure mode this record exists to remove.
  //
  // The previous form was `sizeof(kPairs)/sizeof(kPairs[0]) * 2 - 1 == kFrozenLaneCount`. It
  // passed a kPairs whose sidechain entry had been replaced by a second copy of the MIDI pair —
  // sidechain resolved by nothing, MIDI resolved twice — and, worse, `2p-1` is always ODD, so at
  // six frozen lanes NO kPairs could satisfy it while its message told the maintainer to add one.
  // A guard whose instructions cannot be followed gets deleted, which is the opposite of a guard.
  static_assert(pairSetOf(kPairs) == kAllFrozenLanes,
                "kPairs must cover every frozen lane exactly once, by name");

  const auto trackById = [&](uint32_t id) -> const RoutingTrackInput* {
    for (const auto& track : tracks) {
      if (track.trackId == id) {
        return &track;
      }
    }
    return nullptr;
  };

  for (const Pair& pair : kPairs) {
    const RoutingMedia media = laneMedia(pair.input);

    // A DESTINATION IS EITHER MASTER OR A TRACK, and it is TYPED as that rather than encoded as a
    // reserved track id.
    //
    // This was `constexpr uint32_t kMaster = 0xFFFFFFFFu` in the same std::set as real track ids,
    // and a reviewer walked straight through it: a track whose audio_out named track 0xFFFFFFFF —
    // an id the document parser accepts, `project_file.cpp` reading track_id with no upper bound —
    // compiled to an edge with destIsMaster=1. The row's frozen effect is literally
    // `declare_one_track_destination_WITHOUT_DIRECT_MASTER`, the named destination got nothing,
    // master was summed with audio it should never have had, and the edge's latency charge dropped
    // from one block to zero. The Master-plus-Track cardinality violation went undetected in the
    // same way.
    //
    // A sentinel inside a value's own domain is only safe while nobody can produce it. Making the
    // distinction a FIELD costs one bool and cannot be produced at all.
    struct Destination {
      bool isMaster = false;
      uint32_t trackId = 0;
      bool operator<(const Destination& other) const {
        return std::tie(isMaster, trackId) < std::tie(other.isMaster, other.trackId);
      }
    };
    const auto masterDestination = []() {
      Destination d;
      d.isMaster = true;
      return d;
    };
    const auto trackDestination = [](uint32_t id) {
      Destination d;
      d.trackId = id;
      return d;
    };
    std::map<uint32_t, std::set<Destination>> destinationsOf;  // source -> destinations
    std::map<uint32_t, std::set<uint32_t>> sourcesOf;          // destination -> sources

    if (pair.complementary) {
      for (const auto& track : tracks) {
        // A SWITCH, NOT AN if/else CHAIN, and the difference is the whole point of the flag on
        // this target. `-Werror=switch` cannot see a chain: a new TrackRouteKind fell through both
        // arms and contributed nothing to the graph, silently, while the header claimed a new kind
        // was a compile error.
        const TrackRoute& outRoute = laneOf(track.routing, pair.output);
        switch (outRoute.kind) {
          case TrackRouteKind::Track:
            destinationsOf[track.trackId].insert(trackDestination(outRoute.trackId));
            sourcesOf[outRoute.trackId].insert(track.trackId);
            break;
          case TrackRouteKind::Master:
            destinationsOf[track.trackId].insert(masterDestination());
            break;
          case TrackRouteKind::None:
            // `output_none`: "None contributes no output-side edge".
            break;
          case TrackRouteKind::ExternalInput:
            // Not a legal output-side kind; phase 1 refused it before this loop ran.
            break;
        }
      }
    }

    for (const auto& track : tracks) {
      const TrackRoute& inRoute = laneOf(track.routing, pair.input);
      switch (inRoute.kind) {
        case TrackRouteKind::Track:
          // The input side names its source. Coalesces with a matching output declaration rather
          // than duplicating it — `exact_duplicate: coalesce_once` — because both maps are SETS.
          destinationsOf[inRoute.trackId].insert(trackDestination(track.trackId));
          sourcesOf[track.trackId].insert(inRoute.trackId);
          break;
        case TrackRouteKind::ExternalInput:
          built.externals.push_back(RoutingExternalSource{track.trackId, media, inRoute.inputId});
          break;
        case TrackRouteKind::None:
          // `input_none`: "None contributes no input-side constraint".
          break;
        case TrackRouteKind::Master:
          // Not a legal input-side kind on any lane; phase 1 refused it.
          break;
      }
    }

    // `source_output_cardinality`: "For each media lane a source resolves to at most one Track or
    // Master destination; two input-side declarations naming one source for different
    // destinations, or Master plus Track, fail compilation."
    if (pair.complementary) {
      for (const auto& [source, destinations] : destinationsOf) {
        if (destinations.size() > 1) {
          const Destination& other = *std::next(destinations.begin());
          return failCardinality(error, laneMedia(pair.output), source, other.isMaster,
                                 other.trackId);
        }
      }
    }

    // `input_track`: "fails if any different Track source or Master/ExternalInput source also
    // targets the lane" — the final source set must be exactly the one named.
    // `input_external`: "is the sole main input source and conflicts with every Track edge
    // targeting that lane."
    for (const auto& track : tracks) {
      const TrackRoute& inRoute = laneOf(track.routing, pair.input);
      const auto found = sourcesOf.find(track.trackId);
      const size_t incoming = found == sourcesOf.end() ? 0 : found->second.size();
      if (inRoute.kind == TrackRouteKind::Track && incoming > 1) {
        for (uint32_t source : found->second) {
          if (source != inRoute.trackId) {
            return fail(error, RoutingErrorCode::InputConflictsWithSource, pair.input,
                        track.trackId, source);
          }
        }
      }
      if (inRoute.kind == TrackRouteKind::ExternalInput && incoming > 0) {
        return fail(error, RoutingErrorCode::InputConflictsWithSource, pair.input, track.trackId,
                    *found->second.begin());
      }
    }

    // ---- edges ------------------------------------------------------------------------------
    for (const auto& [source, destinations] : destinationsOf) {
      for (const Destination& destination : destinations) {
        RoutingEdge edge;
        edge.media = media;
        edge.sourceTrackId = source;
        edge.sourceBus = 0;
        edge.destIsMaster = destination.isMaster;
        edge.destTrackId = destination.isMaster ? 0 : destination.trackId;
        // `pre_fader_rule`: "preFaderSend selects the N-1 pre/post-fader signal only for audio_out
        // Track and is canonicalized true for every other row."
        //
        // NOTE WHAT THAT EXCLUDES. An edge created only by the INPUT side — B.audioIn = Track(A)
        // while A.audioOut is None — is not an `audio_out Track` row, so it is canonically true
        // even if A authored false. The authored flag describes A's OWN output declaration, and A
        // did not make one.
        const RoutingTrackInput* sourceTrack = trackById(source);
        const bool isAudioOutTrackRow =
            media == RoutingMedia::Audio && !edge.destIsMaster && sourceTrack != nullptr &&
            sourceTrack->routing.audioOut.kind == TrackRouteKind::Track;
        edge.preFaderSend = isAudioOutTrackRow ? sourceTrack->routing.preFaderSend : true;
        built.edges.push_back(edge);
      }
    }
  }

  // ---- PHASE 3: aux children, which are DERIVED rather than authored --------------------------
  //
  // `aux_child_rule`: "Aux children are derived output-bus projections owned by their parent track
  // plan, not authored TrackRoute lanes; they inherit the parent's exact EXECUTION identity and
  // sourceBus participates in deterministic fan-in ordering."
  //
  // THE WORD "EXECUTION" IS LOAD-BEARING and an earlier version of this quotation dropped it. An
  // aux child in this product IS a real track: daw_engine_main.cpp gives it its own id, and
  // engine_consumer.cpp gives it its own gain, pan, mute and solo. What it INHERITS is the
  // parent's execution identity — the SHM view, the host gates, the dispatch. Quoting it as "the
  // parent's exact identity" turns a narrow claim into "the child is the parent", which is exactly
  // the reading RoutingAuxChild{parentTrackId, busIndex} embodies by carrying no child id. That
  // modelling choice is right for ROUTING, where a stem's edges are the parent's; dropping the
  // qualifier launders it into something the record does not say.
  //
  // So a child contributes the same edges its parent does, with the parent's id and the child's
  // bus. It is not a sixth lane and it authors nothing; copying the parent's TrackRouting onto the
  // child — which is what the engine does today — makes it a track that declares things.
  {
    const size_t authored = built.edges.size();
    for (const auto& child : auxChildren) {
      if (child.busIndex == 0) {
        continue;  // bus 0 IS the parent's main output; it is already an edge above
      }
      for (size_t i = 0; i < authored; ++i) {
        const RoutingEdge& parentEdge = built.edges[i];
        if (parentEdge.sourceTrackId != child.parentTrackId || parentEdge.sourceBus != 0) {
          continue;
        }
        // AUDIO ONLY, and the clause says so twice: an aux child is a derived "OUTPUT-BUS
        // projection", and an output bus in this product is audio — auxBusChannelOffset and
        // auxBusChannelCount locate its CHANNELS in the parent's output plane, which
        // engine_produce_block.cpp:631 describes as "32 channels = up to 16 stereo stems".
        //
        // Without this filter the projection copied the parent's MIDI and sidechain edges too, one
        // per bus. A parent with a 3-bus multi-out instrument sending MIDI to another track
        // delivered every note THREE TIMES — there is no per-bus MIDI stream for sourceBus to
        // select, so all three edges carried the same notes. The sidechain case is different and
        // an earlier version of this comment got it wrong: the three key edges would carry
        // DIFFERENT content, since auxBusChannelOffset/auxBusChannelCount make each bus a distinct
        // channel range, so a keyed compressor would have summed three stems rather than tripling
        // one signal. Still wrong, and wrong in a way that is harder to hear: the engine pulls a
        // key from the source's main output plane (engine_produce_block.cpp), which is bus 0 and
        // only bus 0. A stem is a slice of the parent's audio output; it is not another copy of
        // everything the parent sends.
        //
        // AND THIS IS NOT MIDI-PER-BUS, which is the first objection to reach for. That feature
        // (engine_produce_block.cpp:615) runs the OTHER WAY: a child's OWN authored notes are
        // rendered into the PARENT's host ring, tagged with the child's bus MIDI channel, so a
        // multitimbral instrument steers channel k to output bus k. It is inbound content, not a
        // routing edge, and the child authors it rather than inheriting it. Nothing about it wants
        // the parent's outbound MIDI duplicated per bus.
        if (parentEdge.media != RoutingMedia::Audio) {
          continue;
        }
        RoutingEdge derived = parentEdge;
        derived.sourceBus = child.busIndex;
        built.edges.push_back(derived);
      }
    }
  }

  // ---- PHASE 4: the reduce order -------------------------------------------------------------
  //
  // "Fan-in reduces in ascending {sourceTrackId, sourceBus, channel} order." Channels are iterated
  // inside an edge by whoever consumes this, so ordering the edges of one {destination, media} by
  // {sourceTrackId, sourceBus} makes the FLATTENED sequence exactly that triple. The destination
  // and media lead the key so that one destination's fan-in is contiguous.
  std::sort(built.edges.begin(), built.edges.end(),
            [](const RoutingEdge& a, const RoutingEdge& b) {
              return std::tie(a.destIsMaster, a.destTrackId, a.media, a.sourceTrackId, a.sourceBus)
                   < std::tie(b.destIsMaster, b.destTrackId, b.media, b.sourceTrackId, b.sourceBus);
            });
  std::sort(built.externals.begin(), built.externals.end(),
            [](const RoutingExternalSource& a, const RoutingExternalSource& b) {
              return std::tie(a.trackId, a.media, a.inputId)
                   < std::tie(b.trackId, b.media, b.inputId);
            });
  return succeed();
}

}  // namespace daw
