#include "apps/patcher_preset_library.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace daw {
namespace {

void setError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

std::string defaultPatcherPresetDir() {
  const char* env = std::getenv("DAW_PATCHER_PRESET_DIR");
  if (env && *env) {
    return std::string(env);
  }
  const std::filesystem::path primary = std::filesystem::path("presets") / "patcher";
  if (std::filesystem::exists(primary)) {
    return primary.string();
  }
  const std::filesystem::path fallback = std::filesystem::path("..") / "presets" / "patcher";
  if (std::filesystem::exists(fallback)) {
    return fallback.string();
  }
  return primary.string();
}

bool discoverPatcherPresets(const std::string& dir,
                            std::vector<PatcherPresetInfo>& outPresets,
                            std::string* error) {
  outPresets.clear();
  std::filesystem::path root(dir);
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    setError(error, "patcher preset directory does not exist");
    return false;
  }
  if (!std::filesystem::is_directory(root, ec)) {
    setError(error, "patcher preset path is not a directory");
    return false;
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec) {
      setError(error, "error scanning patcher preset directory");
      return false;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    PatcherPresetInfo info;
    info.name = entry.path().stem().string();
    info.path = entry.path().string();
    outPresets.push_back(std::move(info));
  }

  std::sort(outPresets.begin(), outPresets.end(),
            [](const PatcherPresetInfo& a, const PatcherPresetInfo& b) {
              return a.name < b.name;
            });
  return true;
}

bool listPatcherPresetNames(std::vector<std::string>& outNames,
                            std::string* error) {
  std::vector<PatcherPresetInfo> presets;
  const std::string dir = defaultPatcherPresetDir();
  if (!discoverPatcherPresets(dir, presets, error)) {
    return false;
  }
  outNames.clear();
  outNames.reserve(presets.size());
  for (const auto& preset : presets) {
    outNames.push_back(preset.name);
  }
  return true;
}

}  // namespace daw
