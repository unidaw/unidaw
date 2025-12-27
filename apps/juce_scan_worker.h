#pragma once

#include <string>
#include <vector>

namespace daw {

struct ScanConfig {
  std::string executablePath;
  std::string outPath = "plugin_cache.json";
  std::vector<std::string> paths;
  int timeoutMs = 5000;
  bool forceRescan = false;
  bool childMode = false;
  std::string childPath;
};

ScanConfig parseScanArgs(int argc, char** argv);
int runScan(const ScanConfig& config);

}  // namespace daw
