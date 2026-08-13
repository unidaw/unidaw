#pragma once

// WHICH BUFFER EACH PLUGIN IN A SEGMENT WRITES INTO — the arithmetic, alone and askable.
//
// Adjacent hosted plugins in one segment hand audio along by REBINDING, not copying:
// `inputPtrs = outputPtrs` at the bottom of the per-plugin loop. That reads as in-place aliasing —
// plugin N+1 writing its output into the buffer it is reading — and it is not, because the output
// buffer alternates A/B by parity while the input is always the PREVIOUS iteration's output, which
// has the opposite parity. The two never coincide. See P2-G4-01's step 1.
//
// EXTRACTED BECAUSE THE PROPERTY WAS UNASKABLE. The selection lived inline in a loop inside
// `juce_host_process_main.cpp`, a separate process binary that no test target links, so "input and
// output are never the same buffer" could only be established by reading. That is the same problem
// the undo recording decision had, and the same repair: a pure function, so the guard is a test
// rather than a paragraph.
//
// The safety here is a property of the ARITHMETIC, not of the pointers — which is why this is worth
// pinning: someone simplifying the parity away would still produce working audio for a two-plugin
// chain (A then out) and would break three.
#include <cstdint>

namespace daw::host {

// WHY RELATIVE PARITY AND NOT ABSOLUTE. `(index - segmentStart) % 2` and `index % 2` are BOTH
// safe: either alternates, so neither ever lets a plugin write into the buffer it is reading. A
// negative control substituting the absolute form does not fire, and that is CORRECT rather than a
// gap — the safety property is preserved and only the buffer IDENTITY changes, which nothing
// observes. Recorded so the next person does not widen the test until it fires and thereby pin an
// incidental choice as if it were the contract.
//
// Relative is kept because it makes the segment self-contained: a segment's first plugin always
// writes A regardless of where the segment starts, which is what the spelled-out shape in the test
// reads as. That is a readability argument, not a correctness one, and it is labelled as such.

// The three destinations a plugin in a segment can write to.
enum class ChainBuffer { A, B, SegmentOutput };

// `index` is absolute; `segmentStart` and `segmentEnd` bound the segment (end exclusive).
constexpr ChainBuffer chainOutputFor(uint32_t index, uint32_t segmentStart, uint32_t segmentEnd) {
  if (index + 1 == segmentEnd) {
    return ChainBuffer::SegmentOutput;
  }
  return ((index - segmentStart) % 2 == 0) ? ChainBuffer::A : ChainBuffer::B;
}

// What a plugin READS. The first reads the segment's input; everyone else reads what the plugin
// before it wrote. Expressed as its own function rather than derived at the call site, so the
// "opposite parity" claim is checkable instead of implied.
enum class ChainInput { SegmentInput, A, B };

constexpr ChainInput chainInputFor(uint32_t index, uint32_t segmentStart, uint32_t segmentEnd) {
  if (index == segmentStart) {
    return ChainInput::SegmentInput;
  }
  switch (chainOutputFor(index - 1, segmentStart, segmentEnd)) {
    case ChainBuffer::A:             return ChainInput::A;
    case ChainBuffer::B:             return ChainInput::B;
    case ChainBuffer::SegmentOutput: return ChainInput::SegmentInput;  // unreachable: only the
                                                                       // last writes there, and
                                                                       // nothing follows it
  }
  return ChainInput::SegmentInput;
}

// THE PROPERTY THIS FILE EXISTS FOR. True when a plugin's output buffer is not the one it is
// reading — which is what makes the rebind safe, and what a simplified parity would break.
constexpr bool outputDiffersFromInput(uint32_t index, uint32_t segmentStart, uint32_t segmentEnd) {
  const ChainBuffer out = chainOutputFor(index, segmentStart, segmentEnd);
  const ChainInput in = chainInputFor(index, segmentStart, segmentEnd);
  if (out == ChainBuffer::A) return in != ChainInput::A;
  if (out == ChainBuffer::B) return in != ChainInput::B;
  return true;  // the segment output is never an input to anything in this segment
}

}  // namespace daw::host
