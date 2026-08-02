// Bodies for apps/engine_project_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_project_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include "apps/patcher_preset_library.h"
#include <filesystem>

namespace daw::engine {

void handleSaveProject(ProjectCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& projectLoadOk = deps.projectLoadOk;
  auto& projectLoadSeq = deps.projectLoadSeq;
  const auto& saveProjectToPath = deps.saveProjectToPath;
  const auto& loadProjectFromPath = deps.loadProjectFromPath;
  {
  daw::UiPatcherPresetCommandPayload projectPayload{};
  std::memcpy(&projectPayload, entry.payload, sizeof(projectPayload));
  std::string name(projectPayload.name,
                   strnlen(projectPayload.name, sizeof(projectPayload.name)));
  if (name.empty()) {
    name = "default";
  }
  const std::string dir = daw::defaultProjectDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    daw::LogLine() << "UI: Project failed - cannot create dir " << dir << std::endl;
    return;
  }
  const std::filesystem::path path =
      std::filesystem::path(dir) / (name + ".uniproj.json");
  std::string error;
  if (commandType == daw::UiCommandType::SaveProject) {
    const bool ok = saveProjectToPath(path.string(), &error);
    DAW_EVENT("project.save")
        .field("path", path.string())
        .field("ok", ok)
        .field("error", ok ? std::string() : error);
  } else {
    const bool ok = loadProjectFromPath(path.string(), &error);
    // Publish the result (ok first, then the seq the UI watches) so a failed
    // load is distinguishable from a no-op rather than silently keeping the
    // old project.
    projectLoadOk.store(ok ? 1u : 0u, std::memory_order_release);
    projectLoadSeq.fetch_add(1, std::memory_order_acq_rel);
    DAW_EVENT("project.load")
        .field("path", path.string())
        .field("ok", ok)
        .field("error", ok ? std::string() : error);
  }
  return;
  }
}

}  // namespace daw::engine
