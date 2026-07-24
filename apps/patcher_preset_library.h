#pragma once

#include <string>
#include <vector>

namespace daw {

struct PatcherPresetInfo {
  std::string name;
  std::string path;
};

std::string defaultPatcherPresetDir();

// Directory holding saved projects. Override with DAW_PROJECT_DIR.
std::string defaultProjectDir();

bool discoverPatcherPresets(const std::string& dir,
                            std::vector<PatcherPresetInfo>& outPresets,
                            std::string* error = nullptr);

bool listPatcherPresetNames(std::vector<std::string>& outNames,
                            std::string* error = nullptr);

}  // namespace daw
