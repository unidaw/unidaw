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

PluginCache readPluginCache(const std::string& path);
bool writePluginCacheAtomic(const std::string& path, const PluginCache& cache);
std::string scanStatusToString(ScanStatus status);
ScanStatus scanStatusFromString(const std::string& value);

}  // namespace daw
