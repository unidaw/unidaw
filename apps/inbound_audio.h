#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// AUDIO DELIVERED TO A TRACK BY ANOTHER TRACK'S ROUTE.
//
// AE-P1.2 G2-B item 18, R-ROUTING-AUTHORITY: "Every MIDI, audio, and sidechain Track edge delivers
// the source's fully rendered block N-1 to destination block N ... runtime/worker order cannot
// change same- versus next-block delivery."
//
// THE DEFECT THIS REPLACES. There was ONE buffer per track. A destination consumed it at the start
// of its block and a source wrote it at the end of that same block, so which of the two the render
// pool ran first decided whether the destination heard this block or the next. The engine's own
// comment said so: "Whether the destination sees this block's audio or next block's therefore
// depends on which of the two runs first."
//
// WHY THIS IS A TYPE AND NOT TWO INDEX FUNCTIONS. The first attempt exposed `inboundWriteSlot` and
// `inboundReadSlot` and left the call sites to index a two-element array with them. Independent
// review killed it on two counts, and both are worth keeping written down:
//
//   - NOTHING RATCHETED IT. The unit test could only reach the two index functions, so reverting the
//     CALL SITES left the test green. A test that stays green through the removal of the fix is the
//     shape this codebase already has a name for.
//   - TWO FUNCTIONS CAN BE SWAPPED. Using the write index at the read site and vice versa breaks
//     delivery completely and no test could see it.
//
// THE SECOND IS GONE; THE FIRST IS NARROWED, NOT CLOSED, and saying otherwise was the first version
// of this comment. There is exactly one slot function, it is private, and the two roles differ only
// in WHICH BLOCK THEY NAME -- a source names the block its audio is FOR, a destination names its
// own. One returns a reference and the other takes an out-parameter, so they cannot be swapped. The
// test drives this object, so a revert INSIDE it fails.
//
// What is still unratcheted is the block id the call sites pass. Writing `deliveryBufferFor(blockId
// - 1, ...)` restores same-block delivery, and no registered test sees it: this test links no engine
// code, and the nine registered routing checks assert peaks, energy and log-line counts. Closing
// that needs a check that observes delivery through the producer, which does not exist yet.
//
// WHY THE STAMP, AND NOT PARITY ALONE. Parity keeps a writer and a reader off the same slot within a
// block; it does NOT establish that what is in a slot belongs to the block now reading it. The read
// is below four early returns -- host not ready, realtime lock contended, no shm header, and a ring
// whose mask is zero, which is an ABSENT ring rather than an empty one. Those guards apply to the
// track being processed. A source's write into a DESTINATION is gated by the source's own guards and
// by nothing belonging to the destination, so a destination can skip a block while sources keep
// delivering into it. With parity alone that slot is never cleared and the next reader sums audio from two
// blocks TWO apart -- and when a destination's host dies, `hostReady` stays false and the sum grows
// without bound until it returns. Stamping each slot with the block it is FOR makes stale data
// unreadable instead of silently audible: a destination takes a delivery only if it was addressed to
// the block it is actually rendering.

namespace daw::engine {

class InboundAudio {
 public:
  // A SOURCE FINISHING BLOCK `blockId` delivers into its destination's NEXT block. Returns the
  // accumulation buffer for that block, sized to `samples` and zeroed if this is the first delivery
  // addressed to it -- so several sources fanning into one destination sum, while a slot left behind
  // by a block the destination never consumed is discarded rather than added to.
  //
  // The caller must hold the owning track's inbound mutex.
  std::vector<float>& deliveryBufferFor(uint32_t blockId, size_t samples) {
    const uint32_t target = static_cast<uint32_t>(blockId + 1u);
    const size_t slot = slotOf(target);
    if (!occupied_[slot] || addressedTo_[slot] != target || buffers_[slot].size() != samples) {
      buffers_[slot].assign(samples, 0.0f);
      addressedTo_[slot] = target;
      occupied_[slot] = true;
    }
    return buffers_[slot];
  }

  // A DESTINATION STARTING BLOCK `blockId` takes what was addressed to it, if anything was. Returns
  // true and fills `out` on a delivery; returns false and leaves `out` untouched otherwise, which is
  // the caller's cue to render silence into its input plane. `samples` is the block's expected
  // sample count — a delivery of any other size is not this block's and is discarded.
  //
  // The slot is released either way: a delivery addressed to a block that has now passed is never
  // readable again. The caller must hold the owning track's inbound mutex.
  bool takeDeliveryFor(uint32_t blockId, size_t samples, std::vector<float>& out) {
    const size_t slot = slotOf(blockId);
    // `samples` IS THE AUTHORITY, not out.size(). Comparing the stored buffer against the caller's
    // buffer only ever says "these two disagree" — it cannot say which is wrong, and in the shipped
    // caller it can never fire at all, because that caller resizes `out` to the same expression
    // immediately above under the same lock. A check whose precondition is supplied by its own
    // setup is not a check.
    const bool delivered = occupied_[slot] && addressedTo_[slot] == blockId &&
        buffers_[slot].size() == samples;
    if (delivered && out.size() == samples) {
      out.assign(buffers_[slot].begin(), buffers_[slot].end());
    } else if (delivered) {
      // A delivery of the right size for a caller that asked with the wrong one. Dropping silently
      // is what the previous version did; this cannot happen without a caller bug, so say so.
      occupied_[slot] = false;
      buffers_[slot].clear();
      return false;
    }
    occupied_[slot] = false;
    buffers_[slot].clear();
    return delivered;
  }

  // A TRACK SLOT BEING REUSED. A removed-then-re-added track must not inherit audio delivered to
  // whoever held the id before it; resetTrackContent carries a comment about exactly that class of
  // bug, for routing rather than for this.
  //
  // THIS ONE TAKES THE LOCK ITSELF, unlike the two above, and the asymmetry is deliberate. The other
  // two are called from the producer, which already holds the track's inbound mutex across the whole
  // delivery. This is called from the COMMAND thread inside resetTrackContent, which holds
  // `trackMutex` — a different mutex — while a source that still names this track in its routing
  // snapshot keeps delivering into it, because the write site has no guard on the destination being
  // alive. Clearing the buffers from one thread while another is inside assign() or writing through
  // the returned reference is a use-after-free, and it was introduced by this type: what it replaced
  // was an atomic flag and a vector nothing reset.
  //
  // The order is safe: the producer never takes trackMutex, so trackMutex -> inboundMutex introduces
  // no second ordering.
  void reset(std::mutex& inboundMutex) {
    std::lock_guard<std::mutex> lock(inboundMutex);
    resetLocked();
  }

  // For a runtime that is not published yet — setupTrackRuntime builds it behind a unique_ptr no
  // other thread can see, so there is nothing to lock against and nothing to prove.
  void resetBeforePublication() { resetLocked(); }

 private:
  void resetLocked() {
    for (size_t slot = 0; slot < 2; ++slot) {
      buffers_[slot].clear();
      addressedTo_[slot] = 0;
      occupied_[slot] = false;
    }
  }

  // THE ONLY PLACE A SLOT IS CHOSEN. Two slots and the block's parity keep a write and a read in one
  // block off the same slot, whichever runs first -- and the alternation survives the uint32 wrap
  // BECAUSE 2^32 IS EVEN. If a caller ever passed a counter already reduced modulo an odd number,
  // two consecutive blocks would land on one slot; that is why this takes the raw block id and why
  // the wrap has a test.
  static constexpr size_t slotOf(uint32_t block) { return static_cast<size_t>(block & 1u); }

  std::vector<float> buffers_[2];
  uint32_t addressedTo_[2] = {0, 0};
  bool occupied_[2] = {false, false};
};

}  // namespace daw::engine
