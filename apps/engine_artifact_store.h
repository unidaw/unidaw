#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "apps/artifact_inventory.h"

// WHAT THE ENGINE STILL HOLDS OF EACH PLUGIN'S FILES, and where it got it.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. A save has to answer, per hosted device and per
// side, one question: what bytes go into the new generation? There are exactly three answers, and
// the contract names all three:
//
//   LiveCapture       the host was asked and gave them. The normal case.
//   Schema6Generation the last load read them out of the document's own verified inventory.
//   LegacyOldKey      the last load read them from a schema 1-5 project's `t<track>_d<device>`
//                     path, through the retained LegacyArtifactKey.
//
// And one non-answer: ExplicitAbsent — there is no such file, said deliberately.
//
// THE FOURTH ANSWER IS THE ONE THAT MUST NOT EXIST: "there is a canonical-looking file at the path
// I would have written". `save_rules` says it outright — "unavailable capture emits a structured
// diagnostic and selects retained Present bytes or ExplicitAbsent, NEVER ambient path existence".
// That is why this store holds BYTES rather than paths: a path can be resolved later, against a
// directory that has changed, and produce a plausible answer nobody chose.
//
// WHY IT IS ENGINE STATE AND NOT DOCUMENT STATE. A plugin's opaque blob is not part of the
// authored song — it must not be serialized into project.json, compared by the document comparer,
// or restored by undo (the same argument DocumentHistory makes for its parallel PluginStateSnapshot
// vector). It lives beside the document, and this is what remembers it between a load and the next
// save.

namespace daw::engine {

// One side of one device's artifacts.
struct RetainedArtifact {
  daw::ArtifactSource source = daw::ArtifactSource::LiveCapture;
  std::vector<uint8_t> bytes;
};

class ArtifactStore {
 public:
  // Key: {globalDeviceId, kind}. The device id is project-globally unique, so no track is needed —
  // which is the point of R-DEVICE-ID-LIFETIME, and why a device that moves tracks keeps its
  // artifacts.
  using Key = std::pair<uint32_t, daw::ArtifactKind>;

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    retained_.clear();
  }

  void retain(uint32_t deviceId, daw::ArtifactKind kind, daw::ArtifactSource source,
              std::vector<uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    retained_[Key{deviceId, kind}] = RetainedArtifact{source, std::move(bytes)};
  }

  // ABSENT IS RECORDED BY NOT BEING HERE, and that is deliberate: a side with no entry is
  // ExplicitAbsent. Storing an empty `Present` would make "no file" and "a zero-byte file" the
  // same value, and the contract makes an empty blob a LOAD FAILURE rather than an absence.
  //
  // THERE IS NO `forget`. There was, and it had no callers once the load switched to clearing and
  // repopulating the whole store from the verified inventory — at which point a per-key eraser was
  // a mechanism nothing drove, documented at length as if it were load-bearing.
  //
  // WHAT THAT COSTS, AND WHY IT IS SAFE. A plugin removed from a chain mid-session leaves its bytes
  // here until the next load clears them, so a session that adds and removes N plugins holds N
  // state chunks. It cannot become WRONG, because stable device ids never repeat: the watermark
  // only rises (see apps/engine_device_id_watermark.h), so a removed device's id is never issued
  // again and its bytes can never be attributed to another device. And a removed device is not in
  // document.tracks, so the save's walk never reaches it and it produces no entry. The cost is
  // memory bounded by one session's plugin churn, not a correctness hazard.

  // The retained bytes for this side, or nothing. Returned by value: the caller writes them to a
  // file while other threads may still be editing chains.
  bool lookup(uint32_t deviceId, daw::ArtifactKind kind, RetainedArtifact& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = retained_.find(Key{deviceId, kind});
    if (it == retained_.end()) {
      return false;
    }
    out = it->second;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  std::map<Key, RetainedArtifact> retained_;
};

}  // namespace daw::engine
