#include "apps/engine_artifact_commit.h"

#include <atomic>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "apps/event_log.h"
#include "apps/sha256.h"

namespace daw::engine {
namespace {

bool readWhole(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool writeWhole(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  out.flush();
  return static_cast<bool>(out);
}

}  // namespace

std::string renderParameterManifest(const std::string& pluginName,
                                    uint32_t trackId,
                                    uint32_t deviceId,
                                    const std::vector<daw::HostParamWire>& params) {
  // MOVED VERBATIM from the save loop; the only change is that it builds a string instead of
  // writing to a stream. The byte-for-byte output must not drift, because it is now digested and
  // an inventory built by one version would not verify against a file written by another.
  const auto esc = [](const char* raw, size_t cap) {
    std::string out;
    const size_t n = ::strnlen(raw, cap);
    for (size_t i = 0; i < n; ++i) {
      const char c = raw[i];
      if (c == '"' || c == '\\') {
        out.push_back('\\');
        out.push_back(c);
      } else if (static_cast<unsigned char>(c) >= 0x20) {
        out.push_back(c);
      }
    }
    return out;
  };
  std::ostringstream mf;
  mf << "{\n  \"plugin\": \"" << esc(pluginName.c_str(), pluginName.size())
     << "\",\n  \"track\": " << trackId
     << ",\n  \"device\": " << deviceId << ",\n  \"params\": [\n";
  for (size_t i = 0; i < params.size(); ++i) {
    const auto& w = params[i];
    mf << "    { \"index\": " << w.index
       << ", \"id\": \"" << esc(w.stableId, sizeof(w.stableId))
       << "\", \"name\": \"" << esc(w.name, sizeof(w.name))
       << "\", \"unit\": \"" << esc(w.label, sizeof(w.label))
       << "\", \"value\": " << w.normalized
       << ", \"display\": \"" << esc(w.display, sizeof(w.display))
       << "\", \"min\": \"" << esc(w.minText, sizeof(w.minText))
       << "\", \"max\": \"" << esc(w.maxText, sizeof(w.maxText))
       << "\", \"default\": " << w.defaultNormalized
       << ", \"steps\": " << w.stepCount
       << ", \"discrete\": "
       << ((w.flags & daw::kHostParamDiscrete) ? "true" : "false")
       << ", \"automatable\": "
       << ((w.flags & daw::kHostParamAutomatable) ? "true" : "false")
       << " }" << (i + 1 == params.size() ? "" : ",") << "\n";
  }
  mf << "  ]\n}\n";
  return mf.str();
}

namespace {

// The exact literals renderParameterManifest emits between the plugin name and the params array.
// Written once here so a change to the renderer that forgets this file fails these matchers rather
// than silently making every manifest unrewritable.
constexpr const char* kTrackLead = "\",\n  \"track\": ";
constexpr const char* kDeviceLead = ",\n  \"device\": ";
constexpr const char* kParamsLead = ",\n  \"params\": [";

// Parse a run of decimal digits at `pos`, advancing it. Refuses an empty run, a leading zero on a
// multi-digit number (the renderer never emits one, so accepting it would accept a file the
// renderer could not have written), and anything that would overflow.
bool parseUint32At(const std::string& text, size_t& pos, uint32_t& out) {
  const size_t start = pos;
  uint64_t value = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    value = value * 10 + static_cast<uint64_t>(text[pos] - '0');
    if (value > 0xFFFFFFFFull) {
      return false;
    }
    ++pos;
  }
  if (pos == start) {
    return false;
  }
  if (pos - start > 1 && text[start] == '0') {
    return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

// Locate the embedded pair, returning the byte ranges of the two numbers so a caller can either
// read them or replace them. `false` means the bytes are not a manifest this renderer produced.
bool locateEmbeddedKey(const std::string& text, size_t& trackBegin, size_t& trackEnd,
                       uint32_t& trackId, size_t& deviceBegin, size_t& deviceEnd,
                       uint32_t& deviceId) {
  const size_t lead = text.find(kTrackLead);
  if (lead == std::string::npos) {
    return false;
  }
  trackBegin = lead + std::strlen(kTrackLead);
  size_t pos = trackBegin;
  if (!parseUint32At(text, pos, trackId)) {
    return false;
  }
  trackEnd = pos;
  if (text.compare(pos, std::strlen(kDeviceLead), kDeviceLead) != 0) {
    return false;
  }
  deviceBegin = pos + std::strlen(kDeviceLead);
  pos = deviceBegin;
  if (!parseUint32At(text, pos, deviceId)) {
    return false;
  }
  deviceEnd = pos;
  return text.compare(pos, std::strlen(kParamsLead), kParamsLead) == 0;
}

}  // namespace

bool manifestEmbeddedKey(const std::vector<uint8_t>& bytes, uint32_t& trackId,
                         uint32_t& deviceId) {
  const std::string text(bytes.begin(), bytes.end());
  size_t tb = 0, te = 0, db = 0, de = 0;
  return locateEmbeddedKey(text, tb, te, trackId, db, de, deviceId);
}

bool rewriteManifestEmbeddedKey(std::vector<uint8_t>& bytes, uint32_t trackId, uint32_t deviceId) {
  std::string text(bytes.begin(), bytes.end());
  size_t tb = 0, te = 0, db = 0, de = 0;
  uint32_t oldTrack = 0;
  uint32_t oldDevice = 0;
  if (!locateEmbeddedKey(text, tb, te, oldTrack, db, de, oldDevice)) {
    return false;
  }
  // DEVICE FIRST. Replacing the track number first would move every offset after it, and the
  // device range was measured before that move — the kind of ordering bug that produces a valid
  // file naming the wrong device rather than an error.
  text.replace(db, de - db, std::to_string(deviceId));
  text.replace(tb, te - tb, std::to_string(trackId));
  bytes.assign(text.begin(), text.end());
  return true;
}

bool commitArtifactGeneration(const std::string& stateDir,
                              const std::string& generation,
                              const std::vector<ArtifactToCommit>& files,
                              std::string* error) {
  const auto setError = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };

  const std::filesystem::path finalDir = daw::artifactGenerationDir(stateDir, generation);
  std::error_code ec;

  // ALREADY THERE? BYTE-VERIFY IT, DO NOT REWRITE IT.
  //
  // The directory name is the digest of its contents, so an existing one should already hold
  // exactly these bytes — two saves of unchanged plugin state converge here, which is the point.
  // If it does NOT hold them, something is wrong that quietly overwriting would hide, and the save
  // must say so rather than repair it.
  if (std::filesystem::exists(finalDir, ec)) {
    for (const auto& file : files) {
      std::vector<uint8_t> onDisk;
      if (!readWhole(finalDir / file.entry.leafName(), onDisk)) {
        return setError("generation " + generation + " exists but is missing " +
                        file.entry.leafName());
      }
      if (onDisk != file.bytes) {
        return setError("generation " + generation + " exists and " + file.entry.leafName() +
                        " does not match its digest — the directory name says these are the "
                        "same bytes and they are not");
      }
    }
    DAW_EVENT("artifact.generation_reused")
        .field("generation", generation)
        .field("files", static_cast<uint64_t>(files.size()));
    return true;
  }

  // A FRESH TEMPORARY DIRECTORY, then one atomic rename. A generation is never observed
  // half-written, because it is not at its name until every byte is in it and verified.
  //
  // THE NAME IS UNIQUE PER WRITER, and it has to be. Two engines run against one project directory
  // routinely here, and a staging path named only after the generation is the SAME path for both —
  // so the second writer's remove_all would delete the first writer's half-written directory
  // mid-write. The rename below already reasons about two writers meeting; this is the other place
  // they can, and a shared scratch name would have made that comment true only of the second half.
  static std::atomic<uint64_t> stagingCounter{0};
  const std::filesystem::path tempDir =
      std::filesystem::path(stateDir) /
      ("generations/.staging-" + generation + "-" + std::to_string(::getpid()) + "-" +
       std::to_string(stagingCounter.fetch_add(1, std::memory_order_relaxed)));
  std::filesystem::remove_all(tempDir, ec);
  ec.clear();
  // CREATED, NOT MERELY REQUESTED. create_directories returns false WITHOUT setting ec when the
  // directory already exists, so testing ec alone would let a leftover directory from a crashed
  // attempt ride into the rename with its stale files still in it.
  if (!std::filesystem::create_directories(tempDir, ec) || ec) {
    return setError("cannot create a staging directory for generation " + generation + ": " +
                    (ec ? ec.message() : std::string("it already exists")));
  }

  for (const auto& file : files) {
    if (!writeWhole(tempDir / file.entry.leafName(), file.bytes)) {
      std::filesystem::remove_all(tempDir, ec);
      return setError("cannot write " + file.entry.leafName() + " into generation " + generation);
    }
  }

  // VERIFIED BY READING BACK, not by trusting the write. `save_rules` requires the generation to
  // be written AND verified before the document reference moves; a short write that returned
  // success would otherwise be discovered by the next load, which is the one moment it cannot be
  // repaired.
  for (const auto& file : files) {
    std::vector<uint8_t> onDisk;
    if (!readWhole(tempDir / file.entry.leafName(), onDisk)) {
      std::filesystem::remove_all(tempDir, ec);
      return setError("cannot read back " + file.entry.leafName() + " from generation " + generation);
    }
    if (onDisk.size() != file.entry.size() || daw::sha256Hex(onDisk) != file.entry.sha256()) {
      std::filesystem::remove_all(tempDir, ec);
      return setError(file.entry.leafName() + " does not read back as what was written");
    }
  }

  std::filesystem::rename(tempDir, finalDir, ec);
  if (ec) {
    // CAPTURED HERE, before any cleanup can overwrite `ec`. Every remove_all below takes the same
    // variable, and the one that used to report this error ran after them.
    const std::string renameError = ec.message();
    // ANOTHER SAVE MAY HAVE WON THE RACE, and that is fine: the name is a digest, so whatever is
    // there holds the same bytes. Verify rather than assume — this is the one place two writers
    // can meet, and "it exists so it must be right" is the reasoning this whole record removes.
    if (std::filesystem::exists(finalDir)) {
      for (const auto& file : files) {
        std::vector<uint8_t> onDisk;
        if (!readWhole(finalDir / file.entry.leafName(), onDisk) || onDisk != file.bytes) {
          std::filesystem::remove_all(tempDir, ec);
          return setError("generation " + generation +
                          " appeared during the rename and does not match");
        }
      }
      std::filesystem::remove_all(tempDir, ec);
      return true;
    }
    // THE RENAME'S ERROR, NOT THE CLEANUP'S — captured at the top of this branch. It used to be
    // read after `remove_all(tempDir, ec)` had overwritten `ec` with its own (successful) code, so
    // the message read "cannot publish generation <64 hex>: Undefined error: 0" for every real
    // cause: cross-device rename, ENOSPC, EACCES.
    std::filesystem::remove_all(tempDir, ec);
    return setError("cannot publish generation " + generation + ": " + renameError);
  }

  DAW_EVENT("artifact.generation_committed")
      .field("generation", generation)
      .field("files", static_cast<uint64_t>(files.size()));
  return true;
}

}  // namespace daw::engine
