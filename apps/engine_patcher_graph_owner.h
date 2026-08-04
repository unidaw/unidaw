#pragma once

// THE ASSEMBLED PATCHER GRAPH — the authoritative pool, the copy the RT path reads, and the two
// flags that say what has happened to it.
//
// The fourth engine object of #26. These four appeared as THIRTEEN separate members across six
// *Deps structs. Every module that edits the graph, publishes it, saves it or executes it had to be
// handed the pieces individually.
//
//   patcherGraphState            the authoritative pool, under its own mutex. Edited by commands.
//   patcherGraphSnapshot         the shared_ptr the producer and renderTrack read, swapped
//                                atomically so the RT path never walks a graph mid-edit.
//   patcherAssembledFromDevices  TRUE when every node belongs to exactly one device on exactly one
//                                track. The render path filters on this; the day it could not
//                                reach it, a patcher on one track played another track's
//                                instrument, because the filter had to guess from what it could
//                                see. That is why provenance lives beside the graph.
//   patcherPoolEdited            the pool has an edit the save path must not discard.
//
// WHAT IS DELIBERATELY NOT HERE: patcherParallel and patcherPool — the flag and the worker pool
// that decide HOW the graph is executed. Including them would make the saving 11 instead of 7, and
// would hand UiWriterDeps, LoadProjectDeps, PatcherCommandDeps and SaveProjectDeps a thread pool
// none of them touches. Four of six structs carrying four unused members is the deps structs' own
// mistake inverted, and the same call as leaving tempoProvider out of SongTiming: an object a
// caller must accept whole is only an improvement when the caller wanted most of it.
//
// The split is not arbitrary. WHAT THE GRAPH IS and HOW IT IS RUN are different questions, asked by
// different code at different times — commands and persistence ask the first, the producer asks the
// second, and only the producer asks both.
//
// MEMBER NAMES ARE THE OLD LOCAL NAMES, so every reader moves unchanged.
//
// NOT A LOCK. patcherGraphState carries its own mutex and that is still the only thing serialising
// edits; grouping these four does not make the snapshot and the flags mutually consistent.
#include <atomic>
#include <memory>

#include "patcher_graph.h"

namespace daw::engine {

struct PatcherGraphOwner {
  daw::PatcherGraphState patcherGraphState;
  std::shared_ptr<daw::PatcherGraph> patcherGraphSnapshot;
  std::atomic<bool> patcherAssembledFromDevices{false};
  std::atomic<bool> patcherPoolEdited{false};
};

}  // namespace daw::engine
