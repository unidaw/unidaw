#pragma once
// COMMAND LINE AND STALE-HOST CHECK — everything main() decides before it owns any resource.
//
// This is the first 150 lines of main() and it was the last part anyone would have thought to
// test, because reaching it meant starting an engine. It is a pure function of argv now, so the
// flag rules below are checked directly (apps/engine_startup_tests_main.cpp) rather than inferred
// from whether a suite that starts an engine happened to pass.
//
// THE TWO RULES THAT ARE HERE BECAUSE THEY WERE ONCE WRONG:
//
//   The loop bound is `i < argc`, not `i + 1 < argc`. With the old bound a valueless flag was
//   INVISIBLE when it came last, so `daw_engine --no-spawn` silently spawned a host. Flags that
//   take a value check for one themselves.
//
//   --sample-rate outside 8000..384000 is REFUSED, not clamped and not ignored. A silent fallback
//   to the device rate is how the original defect stayed invisible for weeks: the render would
//   claim to honour a rate it had discarded.
#include <cstdint>
#include <string>

namespace daw::engine {

// What argv decides. Every field is read by main() from here on; nothing else in the engine holds
// a second copy.
struct EngineArgs {
  std::string socketPath;      // --socket; main() seeds the default before parsing
  std::string pluginPath;      // --plugin, made absolute
  bool spawnHost = true;       // --no-spawn
  int runSeconds = -1;         // --run-seconds, clamped at 0
  std::string renderName;      // --render; non-empty means offline
  uint32_t forcedBlockSize = 0;   // --block-size; 0 => take the device's
  double forcedSampleRate = 0.0;  // --sample-rate; 0 => take the device's
  std::string startupProject;     // --project
  bool noAudio = false;  // --no-audio: run everything, output nowhere

  // FROM THE ENVIRONMENT, not from argv. They live in the same struct on purpose: a knob is a
  // startup option whether it arrived as a flag or as an export, and keeping two homes for them
  // is how testMode ended up settable from one and documented in neither.
  bool testMode = false;          // DAW_ENGINE_TEST_MODE
  int testThrottleMs = 0;         // DAW_ENGINE_TEST_THROTTLE_MS
  bool patcherParallel = false;   // DAW_PATCHER_PARALLEL
  bool pdcDisabled = false;       // DAW_DISABLE_PDC
  bool traceNotes = false;        // DAW_TRACE_NOTES
};

// Fills `out`, which the caller has already seeded with its defaults. Returns 0 to continue, or
// the exit code main() should return — an unusable --sample-rate is a refusal, not a warning.
int parseEngineArgs(int argc, char** argv, EngineArgs& out);

// The environment half. Separate from parseEngineArgs only so a test can drive either one alone.
void readStartupEnvironment(EngineArgs& out);

// A STALE HOST BINARY IS DETECTED BEFORE ANY HOST IS SPAWNED, which is the whole point of calling
// this early: it started out just before the threads launch, and that is too late — tracks are set
// up first and that is where hosts connect, so the engine died with 'waitForSocket timed out' and
// the diagnostic never printed.
//
// juce_host_process is a SEPARATE CMake TARGET, so building only daw_engine after a contract change
// leaves a host compiled against the old layout. Two people lost an hour to that on the same day,
// independently. The check is EXACT rather than a heuristic on file times: the host reports the
// versions it was compiled against and exits. Returns 0 to continue, 1 to refuse.
int checkHostBinaryVersion();

}  // namespace daw::engine
