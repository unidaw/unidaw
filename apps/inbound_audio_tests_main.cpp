// THE ONE-BLOCK DELIVERY RULE, exercised on the production object.
//
// AE-P1.2 G2-B item 18, R-ROUTING-AUTHORITY: "Every MIDI, audio, and sidechain Track edge delivers
// the source's fully rendered block N-1 to destination block N ... runtime/worker order cannot
// change same- versus next-block delivery."
//
// AN EARLIER VERSION OF THIS TEST WAS GREEN THROUGH THE REMOVAL OF THE FIX. It called two exposed
// index functions, so reverting the producer's CALL SITES left it passing — independent review
// measured that, and it is the reason InboundAudio exists as a type with a private slot rule instead
// of as two functions the call sites index with. This drives the same object the producer drives,
// so a revert inside it fails here.
//
// The end-to-end check that renders one project twice with the track ids swapped is NOT the
// verification: it is unregistered and declared, because its run-to-run variation is larger than the
// difference it looks for. What it can still do is reproduce; what it cannot do is decide.

#include <cstdio>
#include <string>
#include <mutex>
#include <vector>

#include "apps/inbound_audio.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("inbound_audio_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

constexpr size_t kSamples = 4;

// A source delivering `value` into `audio` at the end of its block `blockId`.
void deliver(daw::engine::InboundAudio& audio, uint32_t blockId, float value) {
  std::vector<float>& buffer = audio.deliveryBufferFor(blockId, kSamples);
  for (size_t i = 0; i < kSamples; ++i) {
    buffer[i] += value;
  }
}

// A destination consuming at the start of its block `blockId`. Returns the first sample, or a
// sentinel when nothing was addressed to that block.
//
// THE SENTINEL IS A HAZARD AND IS FENCED. Folding "nothing arrived" into the float channel means any
// future test value equal to the sentinel would read as an empty delivery. `out` is pre-filled with
// it, so the no-delivery path also PROVES the contract that `out` is left untouched — and every
// sample is checked, not just the first, so a partial copy cannot pass.
constexpr float kNothing = -1.0f;
float consume(daw::engine::InboundAudio& audio, uint32_t blockId, size_t samples = kSamples) {
  std::vector<float> out(samples, kNothing);
  const bool delivered = audio.takeDeliveryFor(blockId, samples, out);
  if (!delivered) {
    for (size_t i = 0; i < samples; ++i) {
      if (out[i] != kNothing) {
        std::printf("inbound_audio_tests: FAIL out was modified on the no-delivery path\n");
        ++failures;
        break;
      }
    }
    return kNothing;
  }
  for (size_t i = 1; i < samples; ++i) {
    if (out[i] != out[0]) {
      std::printf("inbound_audio_tests: FAIL delivery copied sample 0 but not sample %zu\n", i);
      ++failures;
      break;
    }
  }
  return out[0];
}

}  // namespace

int main() {
  // DELIVERY IS TO THE NEXT BLOCK, AND ONLY TO IT. This is the rule itself: what a source writes
  // finishing block N is what the destination hears in block N + 1, and is NOT what it hears in
  // block N. Both halves matter — the first alone would allow dropping the audio entirely.
  {
    daw::engine::InboundAudio audio;
    deliver(audio, 7, 0.5f);
    expect(consume(audio, 7) == kNothing, "block 7's own delivery is not readable by block 7");
    expect(consume(audio, 8) == 0.5f, "and IS readable by block 8");
  }

  // ORDER WITHIN A BLOCK CANNOT CHANGE THE ANSWER, which is the property the serial group used to
  // buy by pinning an order. Consuming before the source delivers, and after it, give the same
  // result for the same block.
  {
    daw::engine::InboundAudio consumeFirst;
    deliver(consumeFirst, 10, 0.25f);            // block 10 delivered, addressed to 11
    const float early = consume(consumeFirst, 11);  // block 11 reads BEFORE its own sources run
    deliver(consumeFirst, 11, 0.75f);               // then a source finishes block 11

    daw::engine::InboundAudio deliverFirst;
    deliver(deliverFirst, 10, 0.25f);
    deliver(deliverFirst, 11, 0.75f);               // source runs BEFORE the destination reads
    const float late = consume(deliverFirst, 11);

    expect(early == 0.25f, "reading before this block's sources run gives block 10's audio");
    expect(late == 0.25f, "and reading after them gives the same, not block 11's");
    expect(early == late, "processing order does not change what block 11 hears");
  }

  // FAN-IN SUMS WITHIN ONE TARGET BLOCK. Several sources into one destination accumulate; the
  // destination hears all of them, once.
  {
    daw::engine::InboundAudio audio;
    deliver(audio, 3, 0.25f);
    deliver(audio, 3, 0.5f);
    expect(consume(audio, 4) == 0.75f, "two sources into one destination sum");
    expect(consume(audio, 4) == kNothing, "and are not delivered a second time");
  }

  // A SKIPPED DESTINATION BLOCK DROPS ITS DELIVERY RATHER THAN CARRYING IT FORWARD. The read sits
  // below four early returns that gate the track being processed, and a source's write into a
  // DESTINATION is gated by nothing belonging to that destination, so this happens for real.
  // Parity alone would leave the
  // stale slot uncleared and sum audio from two blocks TWO apart into a later one; when a
  // destination's host dies, that sum grows without bound until it returns.
  {
    daw::engine::InboundAudio audio;
    deliver(audio, 20, 0.5f);   // addressed to block 21
    // block 21 never runs
    deliver(audio, 22, 0.25f);  // addressed to block 23
    expect(consume(audio, 23) == 0.25f,
           "block 23 hears block 22's audio ALONE — not summed with the block 21 never took");
  }

  // AND A DELIVERY IS NEVER READ BY A BLOCK IT WAS NOT ADDRESSED TO, even one of the right parity.
  // Two blocks apart is the same slot; the stamp is what separates them.
  {
    daw::engine::InboundAudio audio;
    deliver(audio, 30, 0.5f);  // addressed to 31
    expect(consume(audio, 33) == kNothing,
           "block 33 shares a slot with block 31 and must not read what was addressed to 31");
  }

  // THE WRAP. The alternation survives the uint32 counter wrapping BECAUSE 2^32 IS EVEN: 0xFFFFFFFF
  // has parity 1 and 0 has parity 0. Reasoning got this right and nothing tested it. A counter
  // reduced modulo an odd number before the slot was derived would put two consecutive blocks on one
  // slot, and a block would read what the block before it had already taken.
  {
    const uint32_t last = 0xFFFFFFFFu;
    daw::engine::InboundAudio audio;
    deliver(audio, last, 0.5f);  // addressed to block 0, across the wrap
    expect(consume(audio, last) == kNothing, "the last block does not read its own delivery");
    expect(consume(audio, 0u) == 0.5f, "and the block after the wrap does read it");
  }

  // A REUSED TRACK SLOT INHERITS NOTHING. A track removed while a source was routing into it leaves
  // a delivery addressed to a block still to come; resetTrackContent must make it unreadable.
  {
    daw::engine::InboundAudio audio;
    std::mutex inboundMutex;
    deliver(audio, 40, 0.5f);
    audio.reset(inboundMutex);
    expect(consume(audio, 41) == kNothing, "the track reusing this slot hears nothing from before");
  }

  // THE BUFFER IS SIZED TO WHAT WAS ASKED FOR. The header claims it; nothing checked it, and a
  // wrong size would have been undefined behaviour in the helper above rather than a failing test.
  {
    daw::engine::InboundAudio audio;
    expect(audio.deliveryBufferFor(11, 7).size() == 7, "the delivery buffer is sized to `samples`");
    expect(audio.deliveryBufferFor(11, 7).size() == 7, "and stays that size on a second delivery");
  }

  // A CONSUMER ASKING WITH THE WRONG SIZE GETS NOTHING, and the delivery does not survive to
  // ambush a later block. Unreachable from the shipped caller — which is why it needs a test here
  // rather than there.
  {
    daw::engine::InboundAudio audio;
    deliver(audio, 50, 0.5f);
    expect(consume(audio, 51, kSamples + 1) == kNothing, "a wrong-sized read takes nothing");
    expect(consume(audio, 51) == kNothing, "and the delivery is gone, not waiting for the next read");
  }

  // FAN-IN WITH A CHANGED BLOCK SIZE RESTARTS THE ACCUMULATION rather than mixing two lengths. The
  // second source's size wins and the first source's contribution is dropped — the honest behaviour
  // for a block whose size changed mid-flight, and the one a reader should be able to see stated.
  {
    daw::engine::InboundAudio audio;
    audio.deliveryBufferFor(60, kSamples)[0] += 0.5f;
    std::vector<float>& second = audio.deliveryBufferFor(60, kSamples + 2);
    expect(second.size() == kSamples + 2, "the later size wins");
    expect(second[0] == 0.0f, "and the accumulation restarts rather than mixing two lengths");
  }

  if (failures != 0) {
    std::printf("inbound_audio_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("inbound_audio_tests: PASS\n");
  return 0;
}
