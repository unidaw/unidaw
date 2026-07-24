#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

enum class ScanStatus {
  Ok,
  Failed,
  Timeout,
};

struct PluginCacheEntry {
  std::string path;
  std::string pluginIdString;
  std::string pluginUid16;
  std::string name;
  std::string vendor;
  std::string version;
  std::string category;
  bool hasEditor = false;
  bool isInstrument = false;
  int numInputChannels = 0;
  int numOutputChannels = 0;
  int paramCount = 0;
  ScanStatus scanStatus = ScanStatus::Failed;
  std::string error;
  int64_t scanTimeMs = 0;
  int64_t nextRetryAtMs = 0;
};

struct PluginCache {
  int schemaVersion = 1;
  int64_t generatedAtMs = 0;
  std::vector<PluginCacheEntry> entries;
};

// How a saved VstRef was matched back to an entry in the current cache.
enum class VstMatch {
  None,      // nothing matched; the plugin is missing
  Uid16,     // exact plugin identity
  Path,      // same file, identity not recorded or changed
  VendorName,  // moved or reinstalled elsewhere
};

struct VstResolution {
  VstMatch match = VstMatch::None;
  size_t index = 0;
};

// Resolves a saved plugin identity against the current scan, strongest match
// first. Returns match == None when the plugin is not installed.
VstResolution resolveVstRef(const PluginCache& cache,
                            const std::string& uid16,
                            const std::string& path,
                            const std::string& vendor,
                            const std::string& name);

const char* vstMatchToString(VstMatch match);

PluginCache readPluginCache(const std::string& path);
bool writePluginCacheAtomic(const std::string& path, const PluginCache& cache);
std::string scanStatusToString(ScanStatus status);
ScanStatus scanStatusFromString(const std::string& value);

}  // namespace daw
