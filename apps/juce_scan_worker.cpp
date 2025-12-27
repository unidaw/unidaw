#include "apps/juce_scan_worker.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "apps/plugin_cache.h"
#include "platform_juce/juce_wrapper.h"

namespace daw {
namespace {

constexpr int64_t kQuarantineMs = 7LL * 24 * 60 * 60 * 1000;

int64_t nowUnixMs() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
  return ms.count();
}

void printUsage() {
  std::cout << "Usage: juce_scan --out /path/to/plugin_cache.json "
               "--paths /path/to/VST3 [/more/paths...] "
               "[--timeout_ms 5000] [--force_rescan]\n";
}

std::string makeTempResultPath(const std::string& baseOut, int index) {
  std::ostringstream out;
  out << baseOut << ".scan." << index << ".tmp";
  return out.str();
}

bool shouldQuarantineSkip(const PluginCacheEntry& entry,
                          int64_t nowMs,
                          bool forceRescan) {
  if (forceRescan) {
    return false;
  }
  if (entry.scanStatus == ScanStatus::Ok) {
    return false;
  }
  if (entry.nextRetryAtMs <= 0) {
    return false;
  }
  return nowMs < entry.nextRetryAtMs;
}

void applyQuarantine(PluginCacheEntry& entry, int64_t nowMs) {
  if (entry.scanStatus == ScanStatus::Ok) {
    entry.nextRetryAtMs = 0;
    return;
  }
  entry.nextRetryAtMs = nowMs + kQuarantineMs;
}

PluginCacheEntry fromScanResult(const PluginScanResult& result) {
  PluginCacheEntry entry;
  entry.path = result.path;
  entry.pluginIdString = result.identifier;
  entry.pluginUid16 = result.uid16Hex;
  entry.name = result.name;
  entry.vendor = result.vendor;
  entry.version = result.version;
  entry.category = result.category;
  entry.hasEditor = result.hasEditor;
  entry.isInstrument = result.isInstrument;
  entry.numInputChannels = result.numInputChannels;
  entry.numOutputChannels = result.numOutputChannels;
  entry.paramCount = result.paramCount;
  entry.scanTimeMs = result.scanTimeUnixMs;
  entry.scanStatus = result.ok ? ScanStatus::Ok : ScanStatus::Failed;
  entry.error = result.error;
  return entry;
}

int runChildScan(const ScanConfig& config) {
  if (config.childPath.empty()) {
    std::cerr << "Missing --scan-one path." << std::endl;
    return 1;
  }

  auto runtime = createJuceRuntime();
  const auto results = scanVst3File(config.childPath, true, 44100.0, 512);

  PluginCache cache;
  cache.generatedAtMs = nowUnixMs();
  for (const auto& result : results) {
    cache.entries.push_back(fromScanResult(result));
  }

  if (!writePluginCacheAtomic(config.outPath, cache)) {
    std::cerr << "Failed to write scan result." << std::endl;
    return 1;
  }

  return 0;
}

int spawnScanChild(const ScanConfig& config,
                   const std::string& pluginPath,
                   const std::string& resultPath,
                   PluginCache& resultCache) {
  const auto start = std::chrono::steady_clock::now();
  pid_t pid = ::fork();
  if (pid == 0) {
    ::execl(config.executablePath.c_str(),
            config.executablePath.c_str(),
            "--scan-one",
            pluginPath.c_str(),
            "--out",
            resultPath.c_str(),
            nullptr);
    std::perror("execl");
    std::_Exit(127);
  }

  if (pid < 0) {
    return -1;
  }

  while (true) {
    int status = 0;
    const pid_t finished = ::waitpid(pid, &status, WNOHANG);
    if (finished == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        resultCache = readPluginCache(resultPath);
        return 0;
      }
      return 1;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (elapsedMs >= config.timeoutMs) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
      return 2;
    }

    ::usleep(1000 * 10);
  }
}

}  // namespace

ScanConfig parseScanArgs(int argc, char** argv) {
  ScanConfig config;
  if (argc > 0 && argv[0] != nullptr) {
    config.executablePath = argv[0];
  } else {
    config.executablePath = "./juce_scan";
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      config.outPath = argv[++i];
    } else if (arg == "--paths" && i + 1 < argc) {
      config.paths.push_back(argv[++i]);
    } else if (arg == "--timeout_ms" && i + 1 < argc) {
      config.timeoutMs = std::atoi(argv[++i]);
    } else if (arg == "--force_rescan") {
      config.forceRescan = true;
    } else if (arg == "--scan-one" && i + 1 < argc) {
      config.childMode = true;
      config.childPath = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      std::exit(0);
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << std::endl;
    } else {
      config.paths.push_back(arg);
    }
  }
  return config;
}

int runScan(const ScanConfig& config) {
  if (config.childMode) {
    return runChildScan(config);
  }

  if (config.paths.empty()) {
    printUsage();
    return 1;
  }

  const int64_t nowMs = nowUnixMs();
  const auto existingCache = readPluginCache(config.outPath);
  std::unordered_map<std::string, std::vector<PluginCacheEntry>> existingByPath;
  for (const auto& entry : existingCache.entries) {
    existingByPath[entry.path].push_back(entry);
  }

  const auto candidates = discoverVst3Candidates(config.paths);
  PluginCache newCache;
  newCache.generatedAtMs = nowMs;

  int index = 0;
  for (const auto& path : candidates) {
    const auto it = existingByPath.find(path);
    if (it != existingByPath.end()) {
      bool skip = false;
      for (const auto& entry : it->second) {
        if (shouldQuarantineSkip(entry, nowMs, config.forceRescan)) {
          skip = true;
          break;
        }
      }
      if (skip) {
        newCache.entries.insert(newCache.entries.end(), it->second.begin(), it->second.end());
        continue;
      }
    }

    PluginCache childCache;
    const std::string tempResult = makeTempResultPath(config.outPath, index++);
    const int status = spawnScanChild(config, path, tempResult, childCache);
    std::error_code ignore;
    std::filesystem::remove(tempResult, ignore);

    if (status == 0 && !childCache.entries.empty()) {
      for (auto entry : childCache.entries) {
        entry.scanTimeMs = nowMs;
        applyQuarantine(entry, nowMs);
        newCache.entries.push_back(std::move(entry));
      }
      continue;
    }

    PluginCacheEntry failedEntry;
    failedEntry.path = path;
    failedEntry.scanTimeMs = nowMs;
    if (status == 2) {
      failedEntry.scanStatus = ScanStatus::Timeout;
      failedEntry.error = "Scan timeout";
    } else {
      failedEntry.scanStatus = ScanStatus::Failed;
      failedEntry.error = "Scan failed";
    }
    applyQuarantine(failedEntry, nowMs);
    newCache.entries.push_back(std::move(failedEntry));
  }

  if (!writePluginCacheAtomic(config.outPath, newCache)) {
    std::cerr << "Failed to write cache." << std::endl;
    return 1;
  }

  std::cout << "Scanned " << newCache.entries.size() << " plugin(s). Wrote cache to "
            << config.outPath << std::endl;
  return 0;
}

}  // namespace daw
