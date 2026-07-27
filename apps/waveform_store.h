#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "apps/waveform_pyramid.h"

namespace daw {

// UiAudioSource.status values (mirrored in the SHM contract).
constexpr uint32_t kWaveformStatusAbsent = 0;
constexpr uint32_t kWaveformStatusReady = 1;
constexpr uint32_t kWaveformStatusFailed = 2;

// Bumped whenever decodeAudioFile* output changes for any input, INCLUDING the
// downmix rule — it's an input to the content key so a decoder change invalidates
// every cached waveform. See the waveform contract §5 item 7.
constexpr uint32_t kDecoderVersion = 1;

// FNV-1a over a byte range, chainable so a key is built field by field.
inline uint64_t fnv1a64(const void* data, size_t n,
                        uint64_t h = 1469598103934665603ULL) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

// The content key names every input that changes a published bucket value (contract
// §5): the resolved path, the file's size + nanosecond mtime, the decoded frame
// count / sample rate / channel count, and the two version constants. Deliberately
// NOT keyed on clip/placement/gain/fade/tempo — peaks live in the source-frame
// domain, pre-gain and pre-fade, so two placements of one file share one entry.
inline uint64_t computeWaveformContentKey(const std::string& resolvedPath,
                                          uint64_t fileSize, uint64_t mtimeNs,
                                          uint64_t frames, double rateHz,
                                          uint32_t channels,
                                          uint32_t decoderVersion,
                                          uint32_t formatVersion) {
  uint64_t h = fnv1a64(resolvedPath.data(), resolvedPath.size());
  h = fnv1a64(&fileSize, sizeof(fileSize), h);
  h = fnv1a64(&mtimeNs, sizeof(mtimeNs), h);
  h = fnv1a64(&frames, sizeof(frames), h);
  uint64_t rateBits = 0;
  std::memcpy(&rateBits, &rateHz, sizeof(rateBits));  // bit-cast, exact
  h = fnv1a64(&rateBits, sizeof(rateBits), h);
  h = fnv1a64(&channels, sizeof(channels), h);
  h = fnv1a64(&decoderVersion, sizeof(decoderVersion), h);
  h = fnv1a64(&formatVersion, sizeof(formatVersion), h);
  return h;
}

// One decoded audio source, as the UI descriptor table sees it. The pyramid is held
// only for ready sources; a failed decode still gets an entry (with its resolved
// path) so the UI can draw "file missing" instead of an empty box.
struct WaveformSourceEntry {
  uint32_t sourceId = 0;
  uint64_t contentKey = 0;
  std::string path;  // resolved absolute
  uint32_t sourceChannels = 0;
  uint32_t waveChannels = 0;  // min(sourceChannels, 2)
  uint64_t sourceFrames = 0;
  double sourceRateHz = 0.0;
  float absPeak = 0.0f;
  uint32_t levelMask = 0;
  uint32_t status = kWaveformStatusAbsent;
  bool channelsTruncated = false;
  bool clipped = false;
  std::shared_ptr<const WaveformPyramid> pyramid;  // null unless status == ready
};

// The engine-lifetime registry of decoded audio sources for waveform display. It is
// the single owner of the pyramids the RequestWaveform handler slices, keyed by a
// stable sourceId. Its strong references live only for the current project: beginLoad
// drops the previous project's sources (and pyramids) so ten loads don't accumulate
// ten projects' samples — see contract §6. Guarded by a mutex; read on the uiThread
// (never the RT audio callback).
class WaveformStore {
 public:
  // Drop the previous project's sources. Call once at the start of a project load,
  // before the per-track decode funnel repopulates it.
  void beginLoad() {
    std::lock_guard<std::mutex> lock(mu_);
    byId_.clear();
    pathToId_.clear();
    nextId_ = 1;
  }

  // Register a successfully decoded source; returns its stable sourceId. Idempotent
  // for one path within a load: a second placement of the same file reuses the id
  // and keeps the pyramid. A changed content key (a file re-bounced in place) keeps
  // the id but replaces the entry.
  uint32_t internReady(const std::string& resolvedPath, uint64_t contentKey,
                       uint32_t sourceChannels, uint64_t sourceFrames,
                       double sourceRateHz, float absPeak, uint32_t levelMask,
                       bool channelsTruncated, bool clipped,
                       std::shared_ptr<const WaveformPyramid> pyramid) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t id = idForPathLocked(resolvedPath);
    WaveformSourceEntry& e = byId_[id];
    if (e.status == kWaveformStatusReady && e.contentKey == contentKey) {
      return id;  // already interned this exact content
    }
    e.sourceId = id;
    e.contentKey = contentKey;
    e.path = resolvedPath;
    e.sourceChannels = sourceChannels;
    e.waveChannels = pyramid ? pyramid->channels : 0;
    e.sourceFrames = sourceFrames;
    e.sourceRateHz = sourceRateHz;
    e.absPeak = absPeak;
    e.levelMask = levelMask;
    e.status = kWaveformStatusReady;
    e.channelsTruncated = channelsTruncated;
    e.clipped = clipped;
    e.pyramid = std::move(pyramid);
    return id;
  }

  // Register a source that failed to decode: a descriptor with the resolved path and
  // status = failed, no pyramid. A ready entry is never downgraded to failed (a file
  // that decoded for one placement is not "missing" because a later lookup raced).
  uint32_t internFailed(const std::string& resolvedPath) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t id = idForPathLocked(resolvedPath);
    WaveformSourceEntry& e = byId_[id];
    if (e.status == kWaveformStatusReady) {
      return id;
    }
    e.sourceId = id;
    e.path = resolvedPath;
    e.status = kWaveformStatusFailed;
    return id;
  }

  // The sourceId a resolved path was interned under, or 0 if it wasn't. Lets the clip
  // table join a clip's sourcePath to its source descriptor.
  uint32_t sourceIdForPath(const std::string& resolvedPath) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pathToId_.find(resolvedPath);
    return it == pathToId_.end() ? 0u : it->second;
  }

  // A copy of every current entry, for publishing the descriptor table. Pyramids are
  // shared_ptr, so the copy is cheap and keeps them alive for the caller.
  std::vector<WaveformSourceEntry> snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<WaveformSourceEntry> out;
    out.reserve(byId_.size());
    for (const auto& [id, e] : byId_) out.push_back(e);
    return out;
  }

  // Look up one source by id for the request handler; copies the metadata and shares
  // the pyramid under the lock so the caller can slice it without holding the lock.
  bool lookup(uint32_t sourceId, WaveformSourceEntry& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = byId_.find(sourceId);
    if (it == byId_.end()) return false;
    out = it->second;
    return true;
  }

 private:
  uint32_t idForPathLocked(const std::string& resolvedPath) {
    auto it = pathToId_.find(resolvedPath);
    if (it != pathToId_.end()) return it->second;
    const uint32_t id = nextId_++;
    pathToId_.emplace(resolvedPath, id);
    return id;
  }

  mutable std::mutex mu_;
  uint32_t nextId_ = 1;
  std::unordered_map<std::string, uint32_t> pathToId_;
  std::unordered_map<uint32_t, WaveformSourceEntry> byId_;
};

}  // namespace daw
