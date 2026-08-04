#pragma once
// LAYING OUT THE UI SHARED-MEMORY SEGMENT — one function, three arguments, 184 lines of arithmetic.
//
// It creates the segment, computes every region's offset in order, sizes the file to fit, maps it,
// and constructs the five rings in place. The offsets are cumulative, so the ORDER of the
// assignments below is the layout: change one and every region after it moves, which is exactly
// why the whole computation belongs in one place with a name rather than inline in main().
//
// THE BOUNDARY THIS FILE OWNS IS A CONTRACT. Both sides — engine and UI — map the same header and
// compute nothing: the reader trusts these offsets. A mistake here is not a crash, it is a UI
// reading the middle of another ring, which is why kShmVersion exists and why the equality gate
// refuses a mismatched pair rather than trying to cope.
//
// It reads only two things from the engine, which is why it is a plain function and not a Deps
// struct: the base config (block size, sample rate, ring capacity) and the diff ring's capacity.
#include <cstdint>

#include "engine_types.h"
#include "host_controller.h"

namespace daw::engine {

// Fills uiShm (name, fd, base, size, header, rings). Returns 0 on success, or the exit code main()
// should return — a segment that cannot be created or mapped is fatal and says so.
int setUpUiShm(UiShmState& uiShm,
               const daw::HostConfig& baseConfig,
               uint32_t uiDiffRingCapacity);

}  // namespace daw::engine
