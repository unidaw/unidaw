#pragma once

// THE SONG'S METER AND EXTENT — how many beats are in a bar, where the bars fall, how long the
// song is, and the tempo points a project was loaded with.
//
// The third engine object of #26. These six appeared as TWENTY-FOUR separate members across seven
// *Deps structs: LoadProjectDeps wanted all six, ArrangeTimeCommandDeps five, SaveProjectDeps four,
// ConsumerDeps and ProducerBlockDeps three each. Every module that draws a ruler, saves a document
// or answers "which bar is this" had to be handed the pieces individually.
//
// WHY THESE SIX AND NOT ALSO tempoProvider AND tickConverter, which sit next to them in main() and
// would have made the saving 23 instead of 17. Four structs — AudioClipTableDeps, RenderTrackDeps,
// SamplerCommandDeps, AudioRenderRebuildDeps — want ONLY the tempo provider or ONLY the converter
// and nothing else here. Handing them this object would couple each to five things it does not use,
// which is the mistake the deps structs were already making in the other direction. An object that
// a caller must accept whole is only an improvement when the caller wanted most of it: six of the
// seven structs below want two or more of these, and every one of those four wanted exactly one.
//
// THE DISTINCTION IS MEANINGFUL, not just a threshold. Tempo is a RATE and applies to a moment;
// meter is a GROUPING and applies to a span. A sampler needs to know how fast time passes and has
// no opinion about bars.
//
// songEndNanotick is here because it is derived from the same material — the extent of the arranged
// content in the song's own units — and is republished by the same code paths that republish the
// meter.
//
// MEMBER NAMES ARE THE OLD LOCAL NAMES, as with HarmonyTimeline and TransportState, so every reader
// moves unchanged and the diff stays a move.
//
// NOT A LOCK, and nothing here implies the six are consistent with each other. songTimeSigNum and
// songMeter can disagree for a block; the ruler's rule for resolving that lives in
// apps/time_signature_map.h and still does.
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "project_file.h"
#include "time_signature_map.h"

namespace daw::engine {

struct SongTiming {
  daw::TimeSignatureMap songMeter;
  std::shared_ptr<const daw::TimeSignatureMap> meterSnapshot =
      std::make_shared<const daw::TimeSignatureMap>();
  // The song-level default, used where no meter point applies. 4/4 because a project written
  // before mid-song meter existed has no points at all and must keep counting in common time.
  std::atomic<uint32_t> songTimeSigNum{4};
  std::atomic<uint32_t> songTimeSigDen{4};
  std::vector<daw::ProjectTempoPoint> loadedTempoMap{{0, 120.0}};
  std::atomic<uint64_t> songEndNanotick{0};
};

}  // namespace daw::engine
