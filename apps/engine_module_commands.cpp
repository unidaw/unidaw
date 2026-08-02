// Bodies for apps/engine_module_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_module_commands.h"

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

void handleSaveModule(ModuleCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& loadedProjectDir = deps.loadedProjectDir;
  const auto& saveProjectToPath = deps.saveProjectToPath;
  const auto& loadProjectFromPath = deps.loadProjectFromPath;
  (void)loadedProjectDir; (void)saveProjectToPath; (void)loadProjectFromPath;
  (void)entry; (void)header; (void)commandType;
  {
  daw::UiPatcherPresetCommandPayload np{};
  std::memcpy(&np, entry.payload, sizeof(np));
  std::string name(np.name, strnlen(np.name, sizeof(np.name)));
  if (name.empty()) {
    name = "default";
  }
  const std::string dir = daw::defaultProjectDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string modulePath = (std::filesystem::path(dir) / (name + ".uni")).string();
  std::string err;
  if (commandType == daw::UiCommandType::SaveModule) {
    // Assets resolve against the directory the project was LOADED from, which is where the
    // sampler's project-relative names point. Using the save directory instead would work
    // only when they happen to be the same, and fail silently when they are not.
    // SAVE LOOSE FIRST, THEN PACK WHAT WAS SAVED. Not because it is fewer lines — it is
    // more — but because building the document a SECOND way here would be a second answer to
    // "what is this project", and the two would drift. saveProjectToPath already reads LIVE
    // engine state, which is the part that is easy to get wrong; packing its output inherits
    // that for free.
    //
    // It also leaves both forms on disk, which is the model: a directory you edit and diff,
    // a file you send.
    const std::string loosePath =
        (std::filesystem::path(dir) / (name + ".uniproj.json")).string();
    bool ok = saveProjectToPath(loosePath, &err);
    if (ok) {
      daw::ProjectDocument doc;
      ok = daw::loadProject(doc, loosePath, &err);
      if (ok) {
        // Assets resolve against the directory the project was LOADED from, which is where
        // the sampler's project-relative names point. Using the SAVE directory instead would
        // work only when the two happen to coincide, and fail silently when they do not.
        //
        // PLUGIN STATE COMES FROM THE LOOSE SAVE JUST WRITTEN, not from wherever the project
        // was loaded from. saveProjectToPath above wrote every plugin's blob beside
        // `loosePath` moments ago, so that directory holds the CURRENT sound of every device;
        // the load directory may hold a stale copy or none at all.
        ok = daw::saveProjectModule(
            doc, modulePath, loadedProjectDir.empty() ? dir : loadedProjectDir,
            daw::pluginStateDirFor(loosePath), &err);
      }
    }
    DAW_EVENT("project.module_saved")
        .field("path", modulePath)
        .field("ok", ok)
        .field("error", ok ? std::string() : err);
  } else {
    // Unpacked BESIDE the module, into a directory named after it. The unpacked form is an
    // ordinary loose project — the two forms are one document at two levels of packing, so
    // opening a module leaves you working exactly as if you had never packed it.
    const std::string unpackDir = (std::filesystem::path(dir) / name).string();
    daw::ProjectDocument doc;
    const bool ok = daw::loadProjectModule(doc, modulePath, unpackDir, &err);
    if (ok) {
      const std::string docPath =
          (std::filesystem::path(unpackDir) / "project.json").string();
      const bool applied = loadProjectFromPath(docPath, &err);
      DAW_EVENT("project.module_loaded")
          .field("path", modulePath)
          .field("unpacked", unpackDir)
          .field("ok", applied)
          .field("error", applied ? std::string() : err);
    } else {
      DAW_EVENT("project.module_loaded")
          .field("path", modulePath)
          .field("ok", false)
          .field("error", err);
    }
  }
  return;
  }
}

}  // namespace daw::engine
