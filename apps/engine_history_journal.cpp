#include "engine_history_journal.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "engine_types.h"
#include "patcher_preset_library.h"

namespace daw::engine {

std::filesystem::path HistoryJournal::historyPath() const {
    const std::string dir =
        loadedProjectDir_.empty() ? daw::defaultProjectDir() : loadedProjectDir_;
    return std::filesystem::path(dir) / "history.jsonl";
}

void HistoryJournal::historyAppend(const char* op, const char* outcome, uint32_t scopeTrack,
                                   uint32_t baseVersion, const std::string& params) {
    if (std::getenv("DAW_NO_HISTORY")) {
      return;
    }
    std::lock_guard<std::mutex> lock(historyMutex_);
    const auto path = historyPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app);
    if (!out) {
      return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    out << "{\"seq\":" << ++historySeq_ << ",\"ts_ms\":" << now
        << ",\"author\":\"ui\",\"scope\":";
    if (scopeTrack == 0xFFFFFFFFu) {
      out << "\"global\"";
    } else if (scopeTrack == daw::kMasterTrackId) {
      out << "\"master\"";
    } else {
      out << "\"track:" << scopeTrack << "\"";
    }
    out << ",\"base_version\":" << baseVersion << ",\"op\":\"" << op
        << "\",\"outcome\":\"" << outcome << "\",\"params\":{" << params << "}}\n";
}

}  // namespace daw::engine
