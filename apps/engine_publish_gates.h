#pragma once
// WHAT THE CONSUMER HAS ALREADY PUBLISHED — nine gates and the generation counters beside them.
//
// Each published region is rebuilt only when its inputs move: the clip table on clipVersion, the
// automation lane list on automationVersion, the arrangement summary on the section version AND
// the song end. Rewriting a multi-megabyte region every frame is the cost these avoid, and each
// `last*` value is simply what was seen at the previous publish.
//
// THE GENERATIONS ARE NOT THE INPUT VERSIONS, AND THAT DISTINCTION IS LOAD-BEARING. A region's
// published `version` is its OWN generation and starts at 1, so 0 can mean A WRITE IS IN FLIGHT.
// Version-body-version with equality is NOT torn-safe on its own: the number only moves after the
// body is written, so a reader that samples it, reads a body mid-rewrite and samples again before
// the stamp sees v0 == v1 and accepts garbage. The 0 sentinel is what makes the write visible
// while it is happening. That is the arrange summary's actual history.
//
// AND THE GATE MUST COVER EVERYTHING THE REGION CARRIES. The arrangement summary was gated on the
// section version alone while also publishing songEndTick — which changes on a PLACEMENT edit,
// moving no section. Clients kept the song end from the last section edit and no reader could
// tell, because the version they cached on had not moved either. Hence lastArrangeSongEnd sitting
// next to lastArrangeVersion here: two inputs, one gate.
//
// They are TOUCHED ONLY BY THE CONSUMER THREAD, which is why they are plain values rather than
// atomics — and why they were nine loose main() locals threaded through two Deps structs before
// this. warnedPatcherOwnerTooWide is the exception and stays an atomic: it is a one-shot warning
// latch, and the producer reads it.
#include <atomic>
#include <cstdint>

namespace daw::engine {

struct PublishGates {
  // v9: every track's clip in one region, so read-only observers see notes without the request
  // ring. 0xFFFF'FFFF seeds the first publish and a load reruns it.
  uint32_t lastClipAllVersion = 0xFFFF'FFFFu;
  uint32_t lastClipAllQuantizeVersion = 0xFFFF'FFFFu;

  // v28: WHICH PARAMS ARE AUTOMATED — the standing lane list. Automation was writable and
  // unreadable before it: the only lane a UI could offer was one you draw into and never see.
  uint32_t lastAutomationVersion = 0xFFFF'FFFFu;
  uint32_t automationGeneration = 0;

  // M3.25: the ARRANGEMENT SUMMARY — the section spine resolved against the meter, the meter
  // points, and the song end. Rebuilt whole rather than diffed: it is 4 KB and a section reorder
  // changes every entry anyway.
  uint32_t lastArrangeVersion = 0xFFFF'FFFFu;
  uint64_t lastArrangeSongEnd = 0xFFFF'FFFF'FFFF'FFFFull;
  uint32_t arrangeGeneration = 0;

  uint32_t lastClipExtentVersion = 0xFFFF'FFFFu;
  uint32_t lastPatcherVersion = 0xFFFF'FFFFu;

  // A ONE-SHOT WARNING LATCH, and an atomic because the producer reads it too. Once is the point:
  // a per-frame warning about a patcher owner that is too wide would drown the log it belongs in.
  std::atomic<bool> warnedPatcherOwnerTooWide{false};
};

}  // namespace daw::engine
