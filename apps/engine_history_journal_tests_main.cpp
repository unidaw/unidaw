// Tests for apps/engine_history_journal.h — the append-only record of what was asked of the
// document.
//
// NOTHING IN THE SUITE ASSERTED THAT THE ENGINE WRITES THIS FILE AT ALL. daw_lint reads
// history.jsonl and lint_check exercises that reader — against a fixture it writes itself. So the
// READER was covered and the WRITER was not: an engine that journalled nothing, or journalled the
// wrong scope, would have left every check green. I found that by sabotaging historyAppend to a
// no-op and watching the suite pass.
//
// THE SCOPE ENCODING IS THE PART WORTH GUARDING. "global", "master" and "track:N" are three
// branches on one uint32_t, and the two sentinels are 0xFFFFFFFF and kMasterTrackId — numbers that
// read as ordinary track ids if the branch is missed. A journal that files a global undo under
// "track:4294967295" is not obviously broken to anyone reading it later, which is exactly the
// property that makes it worth a test rather than an eyeball.
#include "apps/engine_history_journal.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "apps/engine_types.h"

using namespace daw::engine;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

struct TempDir {
  std::filesystem::path path;
  // PID *AND* A COUNTER. Naming by pid alone made two TempDirs in one process the SAME directory,
  // and the second one's constructor deleted the first one's file — which failed as "the journal
  // did not follow the project" and looked exactly like the defect that test exists to catch.
  static int& counter() {
    static int n = 0;
    return n;
  }
  TempDir() {
    // Removed on the way in as well as out: a leftover from a crashed run must not be read as this
    // run's output.
    path = std::filesystem::temp_directory_path() /
           ("daw_history_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(++counter()));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

// INDEXING A LINE THAT IS NOT THERE MUST FAIL AS AN ASSERTION, NOT A SEGFAULT. The first negative
// control for this file (journal nothing at all) crashed the binary instead of printing which
// expectation broke — a non-zero exit either way, but one of them tells you nothing.
std::string lineAt(const std::vector<std::string>& lines, size_t i) {
  return i < lines.size() ? lines[i] : std::string();
}

std::vector<std::string> readLines(const std::filesystem::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

struct EnvGuard {
  const char* name;
  std::string saved;
  bool had;
  explicit EnvGuard(const char* n) : name(n) {
    const char* v = std::getenv(n);
    had = v != nullptr;
    if (had) saved = v;
  }
  ~EnvGuard() {
    if (had) ::setenv(name, saved.c_str(), 1);
    else ::unsetenv(name);
  }
};

// -------------------------------------------------------------- one line each
void testAppendsOneLinePerCall() {
  EnvGuard g("DAW_NO_HISTORY");
  ::unsetenv("DAW_NO_HISTORY");
  TempDir tmp;
  std::string dir = tmp.path.string();
  HistoryJournal journal{dir};

  journal.historyAppend("add_note", "ok", 0, 5, "\"pitch\":60");
  journal.historyAppend("remove_note", "ok", 1, 6, "");
  const auto lines = readLines(journal.historyPath());
  CHECK(lines.size() == 2u);
  CHECK(journal.sequence() == 2u);

  // THE SEQUENCE IS 1-BASED AND STRICTLY INCREASING — it is what makes the file orderable when the
  // millisecond stamps of two adjacent commands are equal, which for a batch they routinely are.
  CHECK(lineAt(lines, 0).find("\"seq\":1") != std::string::npos);
  CHECK(lineAt(lines, 1).find("\"seq\":2") != std::string::npos);
  CHECK(lineAt(lines, 0).find("\"op\":\"add_note\"") != std::string::npos);
  CHECK(lineAt(lines, 0).find("\"outcome\":\"ok\"") != std::string::npos);
  CHECK(lineAt(lines, 0).find("\"base_version\":5") != std::string::npos);
  CHECK(lineAt(lines, 0).find("\"params\":{\"pitch\":60}") != std::string::npos);
  CHECK(lineAt(lines, 1).find("\"params\":{}") != std::string::npos);

  // IT APPENDS. A second journal over the same directory must not truncate what the first wrote —
  // this is the crash-recovery artifact, and an engine restart is exactly when it matters.
  HistoryJournal second{dir};
  second.historyAppend("undo", "ok", 0xFFFFFFFFu, 7, "");
  CHECK(readLines(journal.historyPath()).size() == 3u);
}

// ------------------------------------------------------------ the three scopes
void testScopeEncoding() {
  EnvGuard g("DAW_NO_HISTORY");
  ::unsetenv("DAW_NO_HISTORY");
  TempDir tmp;
  std::string dir = tmp.path.string();
  HistoryJournal journal{dir};

  journal.historyAppend("undo", "ok", 0xFFFFFFFFu, 0, "");
  journal.historyAppend("set_gain", "ok", daw::kMasterTrackId, 0, "");
  journal.historyAppend("add_note", "ok", 4, 0, "");
  const auto lines = readLines(journal.historyPath());
  CHECK(lines.size() == 3u);

  CHECK(lineAt(lines, 0).find("\"scope\":\"global\"") != std::string::npos);
  CHECK(lineAt(lines, 1).find("\"scope\":\"master\"") != std::string::npos);
  CHECK(lineAt(lines, 2).find("\"scope\":\"track:4\"") != std::string::npos);
  // The sentinels must NOT leak through as track ids — the failure this guards is a global undo
  // filed under track 4294967295, which reads as a real entry to anyone auditing the file later.
  CHECK(lineAt(lines, 0).find("track:") == std::string::npos);
  CHECK(lineAt(lines, 1).find("track:") == std::string::npos);
}

// ------------------------------------------------------------- the kill switch
void testNoHistoryEnvSuppresses() {
  EnvGuard g("DAW_NO_HISTORY");
  TempDir tmp;
  std::string dir = tmp.path.string();
  HistoryJournal journal{dir};

  ::setenv("DAW_NO_HISTORY", "1", 1);
  journal.historyAppend("add_note", "ok", 0, 0, "");
  CHECK(!std::filesystem::exists(journal.historyPath()));
  CHECK(journal.sequence() == 0u);  // the counter does not advance either

  // And it is read per call, not once at construction: turning it off mid-run starts journalling.
  ::unsetenv("DAW_NO_HISTORY");
  journal.historyAppend("add_note", "ok", 0, 0, "");
  CHECK(readLines(journal.historyPath()).size() == 1u);
  CHECK(journal.sequence() == 1u);
}

// -------------------------------------------------------------- the path moves
void testPathFollowsTheProject() {
  EnvGuard g("DAW_NO_HISTORY");
  ::unsetenv("DAW_NO_HISTORY");
  TempDir a, b;
  std::string dir = a.path.string();
  HistoryJournal journal{dir};

  journal.historyAppend("add_note", "ok", 0, 0, "");
  CHECK(readLines(a.path / "history.jsonl").size() == 1u);

  // A PROJECT LOADED AFTER THE ENGINE STARTED MOVES THE JOURNAL WITH IT. The path is resolved per
  // write for exactly this reason; a cached one would keep filing the new project's edits into the
  // previous project's directory, where nobody would think to look for them.
  dir = b.path.string();
  journal.historyAppend("add_note", "ok", 0, 0, "");
  CHECK(readLines(a.path / "history.jsonl").size() == 1u);  // unchanged
  CHECK(readLines(b.path / "history.jsonl").size() == 1u);  // and the new one has it
  // The sequence is the ENGINE's, not the file's: it does not restart when the project changes.
  CHECK(journal.sequence() == 2u);
  CHECK(lineAt(readLines(b.path / "history.jsonl"), 0).find("\"seq\":2") != std::string::npos);
}

// A directory that cannot be written must not take the command down with it.
void testUnwritableDirectoryIsSilent() {
  EnvGuard g("DAW_NO_HISTORY");
  ::unsetenv("DAW_NO_HISTORY");
  std::string dir = "/proc/definitely-not-writable-by-this-test";
  HistoryJournal journal{dir};
  journal.historyAppend("add_note", "ok", 0, 0, "");  // must simply return
  CHECK(journal.sequence() == 0u);
}

}  // namespace

int main() {
  testAppendsOneLinePerCall();
  testScopeEncoding();
  testNoHistoryEnvSuppresses();
  testPathFollowsTheProject();
  testUnwritableDirectoryIsSilent();

  if (g_fail != 0) {
    std::printf("engine_history_journal_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_history_journal_tests: PASS\n");
  return 0;
}
