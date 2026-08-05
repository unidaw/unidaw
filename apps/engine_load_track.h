#pragma once

// ONE TRACK, BUILT FROM THE DOCUMENT IT WAS LOADED FROM.
//
// The first cut out of loadProjectFromPath, which was 1,001 lines and — after renderTrack came
// down from 1,604 to 768 — the largest function in the tree. Unlike renderTrack this one is not a
// pile of lambdas: it is a straight-line PIPELINE, parse then tracks then master then plugins then
// hosts, with each phase inlined at full length. So it comes apart by phase, and this is the
// biggest of them: the body of `for (const auto& source : document.tracks)`.
//
// WHAT ONE TRACK MEANS HERE. The structural store (placements plus copies of the clips they
// reference) is written first, because track.clip and the rails are DERIVED from it and every
// later edit mutates the store rather than the derived copy. Then the mixer, the routing, the
// device chain, automation and mod links, and finally the three snapshots the UI needs to see a
// track that has just appeared.
//
// THE LOOKUP AND THE `continue` STAYED AT THE CALL SITE. A document track with no runtime is
// skipped, and that is a loop decision, not this function's — turning it into an early `return`
// here would read as "this track failed to load" when it means "there is nothing to load it into".
#include "apps/engine_load_project.h"
#include "apps/engine_types.h"
#include "apps/project_file.h"

namespace daw::engine {

// Applies `source` — one track as the document describes it — to `runtime`, and emits the chain,
// routing and mod snapshots for it. `document` is still needed whole because a placement names a
// clip by id and the clips live at document scope.
void loadTrackFromDocument(LoadProjectDeps& deps,
                           TrackRuntime& runtime,
                           const daw::ProjectTrack& source,
                           const daw::ProjectDocument& document);

}  // namespace daw::engine
