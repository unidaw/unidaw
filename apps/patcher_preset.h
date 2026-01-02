#pragma once

#include <string>

#include "apps/patcher_graph.h"

namespace daw {

bool savePatcherPreset(const PatcherGraph& graph,
                       const std::string& path,
                       std::string* error = nullptr);

bool savePatcherPreset(PatcherGraphState& state,
                       const std::string& path,
                       std::string* error = nullptr);

bool loadPatcherPreset(PatcherGraph& graph,
                       const std::string& path,
                       std::string* error = nullptr);

bool loadPatcherPreset(PatcherGraphState& state,
                       const std::string& path,
                       std::string* error = nullptr);

}  // namespace daw
