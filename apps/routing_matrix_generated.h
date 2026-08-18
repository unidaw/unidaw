#pragma once

// GENERATED from the AE-P1.2 G2-B item-18 packet's `routing_matrix` by
// tools/architecture/ae_p1_2_g2b_impl_steps_check.py --write. DO NOT EDIT BY HAND: the
// checker byte-compares this file against the frozen packet, so a hand edit fails rather
// than quietly becoming a second version of the table.
//
// NOT "on every run", which is what this used to say. The comparison is rule 10, and the
// checker returns before it if EITHER packet's pin fails to resolve — a missing worktree,
// a moved HEAD, a changed manifest digest. It fails loudly in that case rather than
// passing, so nothing is certified silently; but the byte-comparison itself is skipped,
// and this file is then only as trustworthy as the last run in which the pins resolved.
//
// T-ROUTING-MATRIX requires the implementation to ITERATE the exact 5x4 matrix. This is
// what it iterates.

#include <cstddef>

namespace daw::generated {

struct RoutingMatrixRow {
  const char* lane;
  const char* kind;
  bool valid;
  const char* effect;
  const char* idRule;
};

inline constexpr const char* kRoutingLanes[] = {"midi_in", "midi_out", "audio_in", "audio_out", "sidechain"};
inline constexpr const char* kRoutingKinds[] = {"None", "Master", "Track", "ExternalInput"};

inline constexpr RoutingMatrixRow kRoutingMatrix[] = {
    {"midi_in", "None", true,
     "no_input_side_declaration", "trackId=0,inputId=0"},
    {"midi_in", "Master", false,
     "reject_master_as_input_source", "rejected"},
    {"midi_in", "Track", true,
     "declare_one_track_source", "existing non-self trackId,inputId=0"},
    {"midi_in", "ExternalInput", true,
     "declare_external_midi_source", "trackId=0,registered nonzero inputId"},
    {"midi_out", "None", true,
     "no_output_side_declaration", "trackId=0,inputId=0"},
    {"midi_out", "Master", false,
     "reject_missing_master_midi_sink", "rejected"},
    {"midi_out", "Track", true,
     "declare_one_track_destination", "existing non-self trackId,inputId=0"},
    {"midi_out", "ExternalInput", false,
     "reject_input_kind_as_output_sink", "rejected"},
    {"audio_in", "None", true,
     "no_input_side_declaration", "trackId=0,inputId=0"},
    {"audio_in", "Master", false,
     "reject_master_as_input_source", "rejected"},
    {"audio_in", "Track", true,
     "declare_one_track_source", "existing non-self trackId,inputId=0"},
    {"audio_in", "ExternalInput", true,
     "declare_external_audio_source", "trackId=0,registered nonzero inputId"},
    {"audio_out", "None", true,
     "no_output_side_declaration", "trackId=0,inputId=0"},
    {"audio_out", "Master", true,
     "declare_master_sink_only", "trackId=0,inputId=0"},
    {"audio_out", "Track", true,
     "declare_one_track_destination_without_direct_master", "existing non-self trackId,inputId=0"},
    {"audio_out", "ExternalInput", false,
     "reject_input_kind_as_output_sink", "rejected"},
    {"sidechain", "None", true,
     "no_key_source", "trackId=0,inputId=0"},
    {"sidechain", "Master", false,
     "reject_master_as_key_source", "rejected"},
    {"sidechain", "Track", true,
     "declare_one_additive_track_key_source", "existing non-self trackId,inputId=0"},
    {"sidechain", "ExternalInput", true,
     "declare_external_audio_key_source", "trackId=0,registered nonzero inputId"},
};

inline constexpr size_t kRoutingMatrixRows = 20;

// The normalization rules, verbatim, so a fixture can quote the sentence it is testing
// rather than paraphrasing it.
//   aux_child_rule: Aux children are derived output-bus projections owned by their parent track
//       plan, not authored TrackRoute lanes; they inherit the parent's exact execution identity
//       and sourceBus participates in deterministic fan-in ordering.
//   complementary_pairs: [midi_out->midi_in, audio_out->audio_in]
//   cycle_policy: Track cycles are valid delayed feedback with one block per edge.
//   exact_duplicate: coalesce_once
//   fan_in_order: [sourceTrackId, sourceBus, channel]
//   input_external: ExternalInput(inputId) is the sole main input source and conflicts with
//       every Track edge targeting that lane.
//   input_none: None contributes no input-side constraint; zero or more output-declared Track
//       sources may fan in.
//   input_track: Track(source) creates that edge when the source output is None, coalesces an
//       exact matching output, and fails if any different Track source or Master/ExternalInput
//       source also targets the lane.
//   output_none: None contributes no output-side edge; one complementary Track input may create
//       the source's sole Track edge.
//   pre_fader_rule: preFaderSend selects the N-1 pre/post-fader signal only for audio_out Track
//       and is canonicalized true for every other row.
//   source_output_cardinality: For each media lane a source resolves to at most one Track or
//       Master destination; two input-side declarations naming one source for different
//       destinations, or Master plus Track, fail compilation.
//   track_edge_latency_blocks: 1

}  // namespace daw::generated
