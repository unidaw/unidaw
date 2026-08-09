#pragma once

// THE ENGINE'S STATE, IN ONE OBJECT.
//
// WHY THIS EXISTS, and why it is not another Deps struct. Thirteen state groups already existed
// — TrackTable, TransportState, SongTiming, ArrangeRail and the rest — each in its own header,
// each documenting what it owns and which lock guards it. That work is done and good. What was
// missing is anything that OWNS them together, and the cost of that shows up in one number: the
// 47 `*Deps` constructions in main() pass 193 distinct names between them, and a deps struct
// that needs six pieces of state has to name all six, at every construction, in the right order.
// `tools/deps_order_check.sh` exists because that order is checkable and was wrong.
//
// A Deps struct answers "what does THIS function need". An engine object answers "what IS the
// engine", and only the second one lets the wiring leave main(): a function taking `EngineState&`
// needs no argument list to keep in sync, so adding a field to a group stops being a change to
// every caller.
//
// WHAT IS DELIBERATELY NOT HERE. The `std::function` adapters (`const std::function<...> xFn = x`)
// and the lambdas they wrap are plumbing, not state, and they belong to the wiring rather than to
// the engine. They go next, and they are the reason main() cannot yet drop below a thousand
// lines: a deps member of type `const std::function<T>&` cannot bind a lambda directly, so each
// one costs a wrapper, and each wrapper costs a line in main() whatever else moves.
//
// CONSTRUCTION ORDER. Every member below is default-constructed and none of them starts a thread
// or touches another, which is what makes hoisting them into one object safe: they used to be
// declared at thirteen different points in main(), spanning 650 lines, and they are now all
// constructed at the first of those points. Verified before the move rather than assumed — a
// group that owned a thread would change behaviour by being constructed earlier.

#include "apps/engine_arrange_rail.h"
#include "apps/engine_aux_child_overlays.h"
#include "apps/engine_clip_window.h"
#include "apps/engine_document_history.h"
#include "apps/engine_loaded_project.h"
#include "apps/engine_patcher_graph_owner.h"
#include "apps/engine_preview_queue.h"
#include "apps/engine_producer_telemetry.h"
#include "apps/engine_publish_gates.h"
#include "apps/engine_render_pool_owner.h"
#include "apps/engine_song_timing.h"
#include "apps/engine_track_table.h"
#include "apps/engine_transport_state.h"
#include "apps/engine_undo_stacks.h"

namespace daw::engine {

struct EngineState {
  // The tracks and the lock that orders access to them. First because most of the rest is about
  // something that happens TO a track.
  TrackTable trackTable;
  // Aux children are DERIVED from a parent's buses, so their authored content is parked here
  // until the derivation asks for it.
  AuxChildOverlays auxChildOverlays;

  // Where the song is, how fast, and in what meter.
  TransportState transport;
  SongTiming songTiming;
  ArrangeRail arrange;

  // What has been loaded, and the window a client is currently asking to see of it.
  LoadedProject loadedProject;
  ClipWindow clipWindow;

  // The patcher pool, assembled from the devices that reference it.
  PatcherGraphOwner patcherGraph;

  // Undo is a whole-store swap, not a per-edit inverse — see the stacks' own header.
  //
  // BEING REPLACED. UndoStacks can only carry TrackStoreState's three fields, which is why 55 of
  // 70 mutating commands are not undoable. documentHistory below is the successor: whole document
  // versions, so there is no subset to omit. Both exist during the changeover; the stacks go when
  // the last writer does.
  UndoStacks undoStacks;

  // UNDO AS A CURSOR over whole-document versions. Redo is the same motion with the sign flipped.
  DocumentHistory documentHistory;

  // Auditioning a note outside the transport.
  PreviewQueue previewQueue;

  // What may be republished and when, and what the producer cost while doing it.
  PublishGates publishGates;
  ProducerTelemetry producerTelemetry;
  RenderPoolOwner renderPoolOwner;
};

}  // namespace daw::engine
