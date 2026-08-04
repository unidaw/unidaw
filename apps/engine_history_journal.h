#pragma once
// history.jsonl — AN APPEND-ONLY JOURNAL OF THE COMMANDS THIS ENGINE ACTED ON.
//
// {seq, ts_ms, author, scope, base_version, op, outcome, params}, one line each. Deliberately NOT
// the DAW_EVENT telemetry stream: that records engine behaviour, this records "what was asked of
// the document, in order", which is what makes it a crash-recovery and what-changed-since-Tuesday
// artifact.
//
// NO INVERSES. Reconstructing 32 correct inverses plus schema-version replay is a project of its
// own; as a record it is nearly free, and it is worth having on those terms rather than not at all.
//
// WRITTEN FROM THE COMMAND THREAD ONLY, because it does file IO — and guarded anyway, so a later
// multi-producer ring cannot interleave half-lines. The mutex, the sequence number and the path
// were three main() locals that only ever moved together; the lock guards the file AND the counter,
// so separating them was never possible in the first place.
//
// The path is RESOLVED PER WRITE, not cached: it follows the loaded project, and a project loaded
// after the engine started must not keep journalling into the previous one's directory.
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace daw::engine {

class HistoryJournal {
 public:
  // loadedProjectDir is main()'s, and it CHANGES when a project is loaded — hence a reference
  // rather than a copy.
  explicit HistoryJournal(const std::string& loadedProjectDir)
      : loadedProjectDir_(loadedProjectDir) {}

  std::filesystem::path historyPath() const;

  // Appends one line. Silent no-op under DAW_NO_HISTORY, and silent if the file cannot be opened:
  // a journal that could not be written must not take the command down with it.
  void historyAppend(const char* op, const char* outcome, uint32_t scopeTrack,
                     uint32_t baseVersion, const std::string& params);

  // The number of lines written this run. Exposed for tests; nothing in the engine reads it.
  uint64_t sequence() const { return historySeq_; }

 private:
  const std::string& loadedProjectDir_;
  std::mutex historyMutex_;
  uint64_t historySeq_ = 0;
};

}  // namespace daw::engine
