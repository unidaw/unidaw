#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace daw::engine {

// UNDO STAGE 5: THE PLUGIN'S OWN STATE, VERSIONED ALONGSIDE THE DOCUMENT.
//
// ProjectDocument is the authored song. A hosted VST's state is NOT in it and never should be: it
// is an opaque blob only the plugin can produce or consume, and it is saved beside the project in
// <project>/.state/*.bin rather than inside the file. So a version of the document is a complete
// record of everything EXCEPT the plugins, and undo restoring only the document is a partial
// restore of a step the user made as one edit.
//
// WHY THE BLOB AND NOT THE PARAMETERS. Owner's ruling on 2026-08-07, and the reasoning is worth
// keeping next to the code: paramMirror holds what the ENGINE has been told about — SetDeviceParam
// writes and automation. A plugin's actual state is more than that (internal modulation, loaded
// samples, editor-only settings, anything changed inside its own window, which reaches no opcode
// at all). Capturing params would restore SOME of a plugin and silently drop the rest — a partial
// restore dressed as a complete one, which is the exact shape of the four subset bugs this whole
// effort exists to kill. requestPluginState returns the plugin's own complete answer to "what are
// you", so that is what a version holds.
//
// DEDUP WITHOUT A HASH, and deliberately without one. A content hash would give dedup for free at
// the cost of a collision restoring the WRONG plugin state — silently, in the one mechanism whose
// entire purpose is not to be silently partial. Instead a version SHARES the previous version's
// shared_ptr when the bytes are equal, so a 100-step history of note edits holds exactly one blob
// per plugin and the comparison that establishes it is an exact memcmp. The cost is one compare
// per capture against a buffer already in memory, which is nothing beside the cross-process round
// trip that produced it.
using PluginBlob = std::shared_ptr<const std::vector<uint8_t>>;

// (trackId, deviceId) — the DURABLE address. Not the host index: that is a position in the live
// chain, and a chain edit renumbers it, which would point a restore at the wrong plugin after
// exactly the operation most likely to need one.
using PluginStateKey = std::pair<uint32_t, uint32_t>;

struct PluginStateSnapshot {
  std::map<PluginStateKey, PluginBlob> blobs;

  // FIDELITY, STATED RATHER THAN ASSUMED. False when some hosted plugin did not answer
  // requestPluginState — host dead, plugin refused, request timed out. Undo still restores
  // everything it has and SAYS the step is partial. "Undo cannot fully restore this step" is a
  // true statement a user can act on; a silent partial restore is not.
  bool complete = true;

  // How many hosted plugins were asked, so "complete" can be distinguished from "there were no
  // plugins to ask" — the two look identical from outside and only one of them is evidence.
  uint32_t asked = 0;
};

// Byte-equality of two snapshots: same devices, same bytes. Used to decide whether a command that
// left the DOCUMENT untouched nevertheless changed something worth an undo step — turning a knob
// on a plugin is exactly that case, and treating it as a no-op would make the single most common
// plugin edit un-undoable.
inline bool samePluginState(const PluginStateSnapshot& a, const PluginStateSnapshot& b) {
  if (a.blobs.size() != b.blobs.size()) {
    return false;
  }
  auto lhs = a.blobs.begin();
  auto rhs = b.blobs.begin();
  for (; lhs != a.blobs.end(); ++lhs, ++rhs) {
    if (lhs->first != rhs->first) {
      return false;
    }
    if (lhs->second == rhs->second) {
      continue;  // shared pointer — already known identical, no compare needed
    }
    if (lhs->second == nullptr || rhs->second == nullptr) {
      return false;
    }
    if (*lhs->second != *rhs->second) {
      return false;
    }
  }
  return true;
}

// Replace each blob in `fresh` with the equal-valued pointer from `previous` where one exists, so
// versions that did not change a plugin share its bytes instead of copying them. Returns `fresh`
// by reference for chaining at a call site that already owns it.
inline void sharePluginBlobsWith(const PluginStateSnapshot& previous,
                                 PluginStateSnapshot& fresh) {
  for (auto& [key, blob] : fresh.blobs) {
    if (blob == nullptr) {
      continue;
    }
    const auto found = previous.blobs.find(key);
    if (found == previous.blobs.end() || found->second == nullptr) {
      continue;
    }
    if (found->second == blob || *found->second == *blob) {
      blob = found->second;
    }
  }
}

}  // namespace daw::engine
