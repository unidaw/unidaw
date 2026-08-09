#pragma once

// ASKING THE PLUGINS WHAT THEY ARE, and telling them what to be again.
//
// The two halves of undo stage 5. Capture runs inside the command bracket, restore runs inside
// undo/redo, and they are here together because they are one contract: whatever capture can read
// is exactly what restore can put back, and a field either side knows about alone is a silent
// partial restore. Keeping them in one file makes that pairing something a reader can check.
//
// OFF THE AUDIO PATH. Both do blocking round trips over the host control socket, on the command
// thread, exactly like save's blob write already does.

#include <cstdint>
#include <memory>

#include "apps/engine_plugin_state_version.h"
#include "apps/engine_state.h"
#include "apps/engine_types.h"

namespace daw::engine {

// Ask every hosted plugin for its state and return one snapshot of the lot.
//
// `onlyDirty` skips a track whose plugins the engine has not seen change since its last capture —
// see TrackRuntime::pluginStateDirty. The blobs of a skipped track come from `previous`, so the
// snapshot is still COMPLETE for that device rather than missing it. Passing false forces a full
// re-read, which is what a fidelity check wants and what the seed after a load does.
PluginStateSnapshot capturePluginState(EngineState& engineState,
                                       std::unique_ptr<TrackRuntime>& masterTrack,
                                       const PluginStateSnapshot& previous,
                                       bool onlyDirty);

// Push back every blob in `snapshot` whose bytes differ from what that plugin last received.
//
// Returns the number of plugins actually pushed. The comparison matters: a note edit changes no
// plugin state, so its version's blobs are the same shared pointers the hosts already hold and
// undo does nothing across the socket at all. Without it every undo would re-push every plugin in
// the project, which is both slow and — for a plugin that resets voices on setState — audible.
uint32_t restorePluginSnapshot(EngineState& engineState,
                               std::unique_ptr<TrackRuntime>& masterTrack,
                               const PluginStateSnapshot& snapshot);

}  // namespace daw::engine
