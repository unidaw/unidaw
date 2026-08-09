#pragma once
// WHAT A LOAD LEFT BEHIND — the clip definitions, their lock, and where the project came from.
//
// Placements live per track (TrackRuntime::sourcePlacements) and reference these clips by id. Save
// re-emits the ones still referenced by a clean track, so the arrangement's structure survives a
// load -> save round trip rather than being flattened into whatever the tracks happen to hold.
//
// loadedProjectDir IS WHY A SAVED PROJECT IS PORTABLE. Sample paths are stored relative to the
// project, so resolving one has to start from the project's own directory rather than the engine's
// working directory — the engine's CWD is wherever it happened to be launched from, which is not a
// property of the document. It is set by loadProjectFromPath before the track loop and read by
// everything that resolves a path afterwards, including the history journal.
//
// The mutex guards the clip vector; the directory is written once per load on the command thread
// and read on it, so it needs no lock of its own. They travel together because everything that
// asks "what was loaded" asks for both.
#include <mutex>
#include <string>
#include <vector>

#include "project_file.h"

namespace daw::engine {

struct LoadedProject {
  std::mutex loadedClipsMutex;
  std::vector<daw::ProjectClip> loadedClips;
  std::string loadedProjectDir;
  // THE FULL PATH, kept because UNDO RE-APPLIES A DOCUMENT AND HAS NO FILE TO NAME.
  //
  // applyDocument derives two things from its `path` argument: this directory, and the plugin
  // state directory (pluginStateDirFor). Undo passed an EMPTY path — it is restoring a document
  // the engine already holds, not opening a file — and an empty path silently means something
  // else entirely: parent_path("") is "", so every relative sample path started resolving against
  // the engine's working directory, and pluginStateDirFor("") is "./.state", so an undone device
  // edit restored the plugin at FACTORY state instead of the state saved beside the project.
  // Neither failed loudly. Undo now passes this, so the document lands where it came from.
  std::string loadedProjectPath;
};

}  // namespace daw::engine
